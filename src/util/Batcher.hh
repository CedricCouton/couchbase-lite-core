//
// Batcher.hh
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

#pragma once
#include "Actor.hh"
#include <memory>
#include <mutex>
#include <vector>

namespace litecore { namespace actor {

    /** A simple queue that adds objects one at a time and sends them to an Actor in a batch. */
    template <class ACTOR, class ITEM>
    class Batcher {
    public:
        using Items = std::unique_ptr<std::vector<Retained<ITEM>>>;

        typedef void (ACTOR::*Processor)();

        /** Constructs a Batcher. Typically done in the Actor subclass's constructor.
            @param actor  The Actor that owns this queue.
            @param processor  The Actor method that should be called to process the queue.
            @param latency  How long to wait before calling the processor, after the first item
                            is added to the queue. */
        Batcher(ACTOR *actor, Processor processor, delay_t latency ={})
        :_actor(*actor)
        ,_processor(processor)
        ,_latency(latency)
        { }

        /** Adds an item to the queue, and schedules a call to the Actor if necessary.
            Thread-safe. */
        void push(ITEM *item) {
            std::lock_guard<std::mutex> lock(_mutex);

            if (!_items) {
                _items.reset(new std::vector<Retained<ITEM>>);
                _items->reserve(200);
            }
            _items->push_back(item);
            if (!_scheduled) {
                _scheduled = true;
                _actor.enqueueAfter(_latency, _processor);
            }
        }

        /** Removes & returns all the items from  the queue, in the order they were added,
            or nullptr if nothing has been added to the queue.
            Thread-safe. */
        Items pop() {
            std::lock_guard<std::mutex> lock(_mutex);

            _scheduled = false;
            return move(_items);
        }

    private:
        ACTOR& _actor;
        Processor _processor;
        delay_t _latency;
        std::mutex _mutex;
        Items _items;
        bool _scheduled {false};
    };

} }
