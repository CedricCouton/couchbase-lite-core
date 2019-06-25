//
// Server.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "LWSServer.hh"
#include "Request.hh"
#include "c4Base.h"
#include <array>
#include <map>
#include <mutex>
#include <functional>
#include <vector>
#include <regex>

struct lws_http_mount;
struct lws_vhost;

namespace litecore { namespace net {
    class LWSResponder;
} }

namespace litecore { namespace REST {

    /** HTTP server, extending LWSServer to add configurable URI handlers. */
    class Server : public net::LWSServer {
    public:
        Server();

        /** Extra HTTP headers to add to every response. */
        void setExtraHeaders(const std::map<std::string, std::string> &headers);

        /** A function that handles a request. */
        using Handler = std::function<void(RequestResponse&)>;

        /** Registers a handler function for a URI pattern.
            Patterns use glob syntax: <http://man7.org/linux/man-pages/man7/glob.7.html>
            Multiple patterns can be joined with a "|".
            Patterns are tested in the order the handlers are added, and the first match is used.*/
        void addHandler(Methods, const std::string &pattern, const Handler&);

        virtual void stop() override;

    protected:
        virtual void dispatchRequest(net::LWSResponder* NONNULL) override;

        struct URIRule {
            Methods     methods;
            std::string pattern;
            std::regex  regex;
            Handler     handler;
        };

        URIRule* findRule(Method method, const std::string &path);
        virtual bool createResponder(lws *client) override;
        ~Server() = default;

    private:
        std::mutex _mutex;
        std::vector<URIRule> _rules;
        std::map<std::string, std::string> _extraHeaders;
    };

} }
