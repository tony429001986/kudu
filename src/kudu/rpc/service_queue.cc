// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/rpc/service_queue.h"

#include <mutex>
#include <optional>
#include <ostream>

#include "kudu/gutil/port.h"

namespace kudu {
namespace rpc {

thread_local LifoServiceQueue::ConsumerState* LifoServiceQueue::tl_consumer_ = nullptr;

LifoServiceQueue::LifoServiceQueue(size_t max_size)
   : max_queue_size_(max_size),
     shutdown_(false) {
  DCHECK_GT(max_queue_size_, 0);
}

LifoServiceQueue::~LifoServiceQueue() {
  DCHECK(queue_.empty())
      << "ServiceQueue holds bare pointers at destruction time";
}

bool LifoServiceQueue::BlockingGet(std::unique_ptr<InboundCall>* out) {
  auto* consumer = tl_consumer_;
  if (PREDICT_FALSE(!consumer)) {
    consumer = tl_consumer_ = new ConsumerState(this);
    std::lock_guard l(lock_);
    consumers_.emplace_back(consumer);
  }

  while (true) {
    {
      std::lock_guard l(lock_);
      if (!queue_.empty()) {
        auto it = queue_.begin();
        out->reset(*it);
        queue_.erase(it);
        return true;
      }
      if (PREDICT_FALSE(shutdown_)) {
        return false;
      }
#if DCHECK_IS_ON()
      consumer->DCheckBoundInstance(this);
#endif
      waiting_consumers_.push_back(consumer);
    }
    InboundCall* call = consumer->Wait();
    if (call != nullptr) {
      out->reset(call);
      return true;
    }
    // if call == nullptr, this means we are shutting down the queue.
    // Loop back around and re-check 'shutdown_'.
  }
}

QueueStatus LifoServiceQueue::Put(InboundCall* call,
                                  std::optional<InboundCall*>* evicted) {
  std::unique_lock l(lock_);
  if (PREDICT_FALSE(shutdown_)) {
    return QUEUE_SHUTDOWN;
  }

  DCHECK(waiting_consumers_.empty() || queue_.empty());

  // fast path
  if (queue_.empty() && !waiting_consumers_.empty()) {
    auto* consumer = waiting_consumers_.back();
    waiting_consumers_.pop_back();
    // Notify condition var(and wake up consumer thread) takes time,
    // so put it out of spinlock scope.
    l.unlock();
    consumer->Post(call);
    return QUEUE_SUCCESS;
  }

  if (PREDICT_FALSE(queue_.size() >= max_queue_size_)) {
    // eviction
    DCHECK_EQ(queue_.size(), max_queue_size_);
    auto it = queue_.end();
    --it;
    if (DeadlineLess(*it, call)) {
      return QUEUE_FULL;
    }

    *evicted = *it;
    queue_.erase(it);
  }

  queue_.insert(call);
  return QUEUE_SUCCESS;
}

void LifoServiceQueue::Shutdown() {
  std::lock_guard l(lock_);
  shutdown_ = true;

  // Post a nullptr to wake up any consumers which are waiting.
  for (auto* cs : waiting_consumers_) {
    cs->Post(nullptr);
  }
  waiting_consumers_.clear();
}

std::string LifoServiceQueue::ToString() const {
  std::string ret;

  std::lock_guard l(lock_);
  for (const auto* t : queue_) {
    ret.append(t->ToString());
    ret.append("\n");
  }
  return ret;
}

} // namespace rpc
} // namespace kudu
