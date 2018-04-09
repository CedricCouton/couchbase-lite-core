//
// WebSocketInterface.cc
//
// Copyright © 2018 Couchbase. All rights reserved.
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

#include "WebSocketImpl.hh"
#include "WebSocketProtocol.hh"
#include "StringUtil.hh"
#include "Timer.hh"
#include <chrono>
#include <functional>
#include <string>

using namespace std;
using namespace fleece;


#pragma mark - WEBSOCKET:


namespace litecore { namespace websocket {

    LogDomain WSLogDomain("WS", LogLevel::Warning);


    atomic_int WebSocket::gInstanceCount;


    WebSocket::WebSocket(const alloc_slice &a, Role role)
    :_url(a)
    ,_role(role)
    {
        ++gInstanceCount;
    }


    WebSocket::~WebSocket() {
        --gInstanceCount;
    }


    void WebSocket::connect(Delegate *delegate) {
        assert(!_delegate);
        assert(delegate);
        _delegate = delegate;
        connect();
    }

} }
