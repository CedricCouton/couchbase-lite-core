//
//  BLIPConnection.cc
//  LiteCore
//
//  Created by Jens Alfke on 12/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "BLIPConnection.hh"
#include "Message.hh"
#include "BLIPInternal.hh"
#include "WebSocketInterface.hh"
#include "Actor.hh"
#include "Logging.hh"
#include "varint.hh"
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <mutex>
#include <vector>

using namespace std;
using namespace fleece;
using namespace litecore;
using namespace litecore::websocket;

namespace litecore { namespace blip {

    static const size_t kDefaultFrameSize = 4096;       // Default size of frame
    static const size_t kBigFrameSize = 16384;          // Max size of frame

    static const size_t kMaxSendSize = 50*1024;         // How much to send at once

    const char* const kMessageTypeNames[8] = {"REQ", "RES", "ERR", "?3?",
                                              "ACKREQ", "AKRES", "?6?", "?7?"};

    LogDomain BLIPLog("BLIP");


    /** Queue of outgoing messages; each message gets to send one frame in turn. */
    class MessageQueue : public vector<Retained<MessageOut>> {
    public:
        MessageQueue()                          { }
        MessageQueue(size_t rsrv)               {reserve(rsrv);}

        bool contains(MessageOut *msg) const    {return find(begin(), end(), msg) != end();}

        MessageOut* findMessage(MessageNo msgNo, bool isResponse) const {
            auto i = find_if(begin(), end(), [&](const Retained<MessageOut> &msg) {
                return msg->number() == msgNo && msg->isResponse() == isResponse;
            });
            return (i != end()) ? *i : nullptr;
        }

        Retained<MessageOut> pop() {
            if (empty())
                return nullptr;
            Retained<MessageOut> msg(front());
            erase(begin());
            return msg;
        }

        bool remove(MessageOut *msg) {
            auto i = find(begin(), end(), msg);
            if (i == end())
                return false;
            erase(i);
            return true;
        }

    };


#pragma mark - BLIP I/O:


    /** The guts of a Connection. */
    class BLIPIO : public Actor, public Logging, public websocket::Delegate {
    private:
        typedef unordered_map<MessageNo, Retained<MessageIn>> MessageMap;

        Retained<Connection>    _connection;
        WebSocket*              _webSocket {nullptr};
        MessageQueue            _outbox;
        MessageQueue            _icebox;
        size_t                  _sentBytes {0};
        MessageMap              _pendingRequests, _pendingResponses;
        atomic<MessageNo>       _lastMessageNo {0};
        MessageNo               _numRequestsReceived {0};
        unique_ptr<uint8_t[]>   _frameBuf;
        std::unordered_map<string, Connection::RequestHandler> _requestHandlers;

    public:

        BLIPIO(Connection *connection, WebSocket *webSocket)
        :Actor(string("BLIP[") + connection->name() + "]")
        ,Logging(BLIPLog)
        ,_connection(connection)
        ,_webSocket(webSocket)
        ,_outbox(10)
        {
            _pendingRequests.reserve(10);
            _pendingResponses.reserve(10);
        }

        void queueMessage(MessageOut *msg) {
            enqueue(&BLIPIO::_queueMessage, Retained<MessageOut>(msg));
        }

        void setRequestHandler(std::string profile, Connection::RequestHandler handler) {
            enqueue(&BLIPIO::_setRequestHandler, profile, handler);
        }

        void close() {
            enqueue(&BLIPIO::_close);
        }

#if DEBUG
        WebSocket* webSocket() const {
            return _webSocket;
        }
#endif

        virtual std::string loggingIdentifier() const override {
            return _connection ? _connection->name() : Logging::loggingIdentifier();
        }


    protected:

        // websocket::Delegate interface:
        virtual void onWebSocketConnect() override {
            _connection->connected();
            onWebSocketWriteable();
        }

        virtual void onWebSocketClose(websocket::CloseStatus status) override {
            enqueue(&BLIPIO::_closed, status);
        }

        virtual void onWebSocketWriteable() override {
            enqueue(&BLIPIO::_onWebSocketWriteable);
        }

        virtual void onWebSocketMessage(slice message, bool binary) override {
            enqueue(&BLIPIO::_onWebSocketMessage, alloc_slice(message), binary);
        }

    private:

        /** Implementation of public close() method. Closes the WebSocket. */
        void _close() {
            if (_webSocket)
                _webSocket->close();
        }

        void _closed(websocket::CloseStatus status) {
            _webSocket = nullptr;
            if (_connection) {
                Retained<BLIPIO> holdOn (this);
                _connection->closed(status);
                _connection = nullptr;
                // TODO: Call error handlers for any unfinished outgoing messages
                _outbox.clear();
                _icebox.clear();
                _pendingRequests.clear();
                _pendingResponses.clear();
                _requestHandlers.clear();
            }
        }


#pragma mark OUTGOING:


        /** Implementation of public queueMessage() method.
            Adds a new message to the outgoing queue and wakes up the queue. */
        void _queueMessage(Retained<MessageOut> msg) {
            if (!_webSocket) {
                log("Can't send request; socket is closed");
                return;
            }
            if (msg->_number == 0)
                msg->_number = ++_lastMessageNo;
            if (!msg->isAck() || BLIPLog.level() <= LogLevel::Verbose) {
                log("Sending %s #%llu, flags=%02x",
                    kMessageTypeNames[msg->type()], msg->_number, msg->flags());
            }
            requeue(msg, true);
        }


        /** Adds a message to the outgoing queue */
        void requeue(MessageOut *msg, bool andWrite =false) {
            assert(!_outbox.contains(msg));
            auto i = _outbox.end();
            if (msg->urgent() && !_outbox.empty()) {
                // High-priority gets queued after the last existing high-priority message,
                // leaving one regular-priority message in between if possible:
                do {
                    --i;
                    if ((*i)->urgent()) {
                        if ((i+1) != _outbox.end())
                            ++i;
                        break;
                    } else if (msg->_bytesSent == 0 && (*i)->_bytesSent == 0) {
                        // But make sure to keep the 1st frames of messages in chronological order:
                        break;
                    }
                } while (i != _outbox.begin());
                ++i;
            }
            _outbox.emplace(i, msg);  // inserts _at_ position i

            if (andWrite)
                writeToWebSocket();
        }
        

        /** Adds an outgoing message to the icebox (until an ACK arrives.) */
        void freezeMessage(MessageOut *msg) {
            logVerbose("Freezing %s #%llu", kMessageTypeNames[msg->type()], msg->number());
            assert(!_outbox.contains(msg));
            assert(!_icebox.contains(msg));
            _icebox.push_back(msg);
        }


        /** Removes an outgoing message from the icebox and re-queues it (after ACK arrives.) */
        void thawMessage(MessageOut *msg) {
            logVerbose("Thawing %s #%llu", kMessageTypeNames[msg->type()], msg->number());
            bool removed = _icebox.remove(msg);
            assert(removed);
            requeue(msg, true);
        }


        /** WebSocketDelegate method -- socket has room to write data. */
        void _onWebSocketWriteable() {
            logVerbose("WebSocket is hungry!");
            _sentBytes = 0;
            writeToWebSocket();
        }


        /** Sends the next frame. */
        void writeToWebSocket() {
            if (_sentBytes >= kMaxSendSize)
                return;

            //logVerbose("Writing to WebSocket...");
            while (_sentBytes < kMaxSendSize) {
                // Get the next message, if any, from the queue:
                Retained<MessageOut> msg(_outbox.pop());
                if (!msg)
                    break;

                FrameFlags frameFlags;
                {
                    // Read a frame from it:
                    size_t maxSize = kDefaultFrameSize;
                    if (msg->urgent() || _outbox.empty() || !_outbox.front()->urgent())
                        maxSize = kBigFrameSize;

                    slice body = msg->nextFrameToSend(maxSize - 10, frameFlags);

                    logVerbose("    Sending frame: %s #%llu, flags %02x, bytes %llu--%llu",
                          kMessageTypeNames[frameFlags & kTypeMask], msg->number(),
                          (frameFlags & ~kTypeMask),
                          (uint64_t)(msg->_bytesSent - body.size),
                          (uint64_t)(msg->_bytesSent - 1));

                    // Copy header and frame to a buffer, and send over the WebSocket:
                    if (!_frameBuf)
                        _frameBuf.reset(new uint8_t[2*kMaxVarintLen64 + kBigFrameSize]);
                    uint8_t *end = _frameBuf.get();
                    end += PutUVarInt(end, msg->_number);
                    end += PutUVarInt(end, frameFlags);
                    memcpy(end, body.buf, body.size);
                    end += body.size;
                    slice frame {_frameBuf.get(), end};
                    _webSocket->send(frame);
                    _sentBytes += frame.size;
                }
                
                // Return message to the queue if it has more frames left to send:
                if (frameFlags & kMoreComing) {
                    if (msg->needsAck())
                        freezeMessage(msg);
                    else
                        requeue(msg);
                } else {
                    if (!msg->isAck() || BLIPLog.level() <= LogLevel::Verbose) {
                        log("Finished sending %s #%llu, flags=%02x",
                            kMessageTypeNames[msg->type()], msg->_number, msg->flags());
                        // Add its response message to _pendingResponses:
                        MessageIn* response = msg->createResponse();
                        if (response)
                            _pendingResponses.emplace(response->number(), response);
                        
                    }
                }
            }
            logVerbose("...Wrote %zu bytes to WebSocket (space left: %lld)",
                       _sentBytes, max(0ll, (int64_t)kMaxSendSize - (int64_t)_sentBytes));
        }


#pragma mark INCOMING:

        
        /** WebSocketDelegate method -- Received a frame: */
        void _onWebSocketMessage(alloc_slice frame, bool binary) {
            if (!binary) {
                log("Ignoring non-binary message");
                return;
            }
            uint64_t msgNo, flagsInt;
            if (!ReadUVarInt(&frame, &msgNo) || !ReadUVarInt(&frame, &flagsInt)) {
                warn("Illegal frame header");
                return;
            }
            auto flags = (FrameFlags)flagsInt;
            logVerbose("Received frame: %s #%llu, flags %02x, length %5ld",
                  kMessageTypeNames[flags & kTypeMask], msgNo,
                  (flags & ~kTypeMask), (long)frame.size);

            Retained<MessageIn> msg;
            auto type = (MessageType)(flags & kTypeMask);
            switch (type) {
                case kRequestType:
                    msg = pendingRequest(msgNo, flags);
                    break;
                case kResponseType:
                case kErrorType: {
                    msg = pendingResponse(msgNo, flags);
                    break;
                case kAckRequestType:
                case kAckResponseType:
                    receivedAck(msgNo, (type == kAckResponseType), frame);
                    break;
                default:
                    log("  Unknown frame type received");
                    break;
                }
            }
            if (msg && msg->receivedFrame(frame, flags)) {
                // Message complete!
                if (type == kRequestType)
                    handleRequest(msg);
                else
                    _connection->delegate().onResponseReceived(msg);
            }
        }


        /** Handle an incoming ACK message, by unfreezing the associated outgoing message. */
        void receivedAck(MessageNo msgNo, bool onResponse, slice body) {
            // Find the MessageOut in either _outbox or _icebox:
            bool frozen = false;
            Retained<MessageOut> msg = _outbox.findMessage(msgNo, onResponse);
            if (!msg) {
                msg = _icebox.findMessage(msgNo, onResponse);
                if (!msg) {
                    //logVerbose("Received ACK of non-current message (%s #%llu)",
                    //      (onResponse ? "RES" : "REQ"), msgNo);
                    return;
                }
                frozen = true;
            }

            uint32_t byteCount;
            if (!ReadUVarInt32(&body, &byteCount)) {
                warn("Couldn't parse body of ACK");
                return;
            }
            
            msg->receivedAck(byteCount);
            if (frozen && !msg->needsAck())
                thawMessage(msg);
        }


        /** Returns the MessageIn object for the incoming request with the given MessageNo. */
        Retained<MessageIn> pendingRequest(MessageNo msgNo, FrameFlags flags) {
            Retained<MessageIn> msg;
            auto i = _pendingRequests.find(msgNo);
            if (i != _pendingRequests.end()) {
                // Existing request: return it, and remove from _pendingRequests if the last frame:
                msg = i->second;
                if (!(flags & kMoreComing))
                    _pendingRequests.erase(i);
            } else if (msgNo == _numRequestsReceived + 1) {
                // New request: create and add to _pendingRequests unless it's a singleton frame:
                ++_numRequestsReceived;
                msg = new MessageIn(_connection, flags, msgNo);
                if (flags & kMoreComing)
                    _pendingRequests.emplace(msgNo, msg);
            } else {
                warn("Bad incoming request number %llu", msgNo);
            }
            return msg;
        }


        /** Returns the MessageIn object for the incoming response with the given MessageNo. */
        Retained<MessageIn> pendingResponse(MessageNo msgNo, FrameFlags flags) {
            Retained<MessageIn> msg;
            auto i = _pendingResponses.find(msgNo);
            if (i != _pendingResponses.end()) {
                msg = i->second;
                if (!(flags & kMoreComing))
                    _pendingResponses.erase(i);
            } else {
                warn("Unexpected response to my message %llu", msgNo);
            }
            return msg;
        }


        void _setRequestHandler(std::string profile, Connection::RequestHandler handler) {
            if (handler)
                _requestHandlers.emplace(profile, handler);
            else
                _requestHandlers.erase(profile);
        }


        void handleRequest(MessageIn *request) {
            try {
                auto profile = request->property("Profile"_sl);
                if (profile) {
                    auto i = _requestHandlers.find(profile.asString());
                    if (i != _requestHandlers.end()) {
                        i->second(request);
                        return;
                    }
                }
                // No handler; just pass it to the delegate:
                _connection->delegate().onRequestReceived(request);
            } catch (...) {
                logError("Caught exception thrown from BLIP request handler");
                request->respondWithError("BLIP"_sl, 501);
            }
        }

    }; // end of class BLIPIO


#pragma mark - CONNECTION:


    Connection::Connection(const Address &address,
                           Provider &provider,
                           ConnectionDelegate &delegate)
    :Logging(BLIPLog)
    ,_name(string("->") + (string)address)
    ,_isServer(false)
    ,_delegate(delegate)
    {
        log("Opening connection...");
        provider.addProtocol("BLIP");
        start(provider.createWebSocket(address));
    }


    Connection::Connection(WebSocket *webSocket,
                           ConnectionDelegate &delegate)
    :Logging(BLIPLog)
    ,_name(string("<-") + (string)webSocket->address())
    ,_isServer(true)
    ,_delegate(delegate)
    {
        log("Accepted connection");
        start(webSocket);
    }


    Connection::~Connection()
    {
        logDebug("~Connection");
    }


    void Connection::start(WebSocket *webSocket) {
        _state = kConnecting;
        webSocket->name = _name;
        _io = new BLIPIO(this, webSocket);
        webSocket->connect(_io);
    }


    /** Public API to send a new request. */
    void Connection::sendRequest(MessageBuilder &mb) {
        Retained<MessageOut> message = new MessageOut(this, mb, 0);
        assert(message->type() == kRequestType);
        send(message);
    }


    /** Internal API to send an outgoing message (a request, response, or ACK.) */
    void Connection::send(MessageOut *msg) {
        _io->queueMessage(msg);
    }


    void Connection::setRequestHandler(string profile, RequestHandler handler) {
        _io->setRequestHandler(profile, handler);
    }


    void Connection::connected() {
        log("Connected!");
        _state = kConnected;
        delegate().onConnect();
    }


    void Connection::close() {
        log("Close connection");
        _state = kClosing;
        _io->close();
    }


    void Connection::closed(CloseStatus status) {
        static const char* kReasonNames[] = {"WebSocket status", "errno", "DNS error"};
        log("Closed with %s %d: %.*s",
              kReasonNames[status.reason], status.code,
              (int)status.message.size, status.message.buf);
        if (status.reason == kWebSocketClose && (status.code == kCodeNormal
                                              || status.code == kCodeGoingAway))
            _state = kClosed;
        else
            _state = kDisconnected;
        _closeStatus = status;
        delegate().onClose(status);
    }


#if DEBUG
    websocket::WebSocket* Connection::webSocket() const {return _io->webSocket();}
#endif

} }
