/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <optional>
#include "message_cache.hpp"

#include <cassert>

#include <qtils/hex.hpp>

#define TRACE_ENABLED 0
#include <libp2p/common/trace.hpp>

namespace libp2p::protocol::gossip {

  MessageCache::MessageCache(Time message_lifetime, TimeFunction clock)
      : message_lifetime_(message_lifetime), clock_(std::move(clock)) {
    assert(message_lifetime_ > Time::zero());
  }

  MessageCache::~MessageCache() = default;

  bool MessageCache::contains(const MessageId &id) const {
    return table_.count(id) != 0;
  }

  std::optional<TopicMessage::Ptr> MessageCache::getMessage(
      const MessageId &id) const {
    auto it = table_.find(id);
    if (it == table_.end()) {
      TRACE(
          "MessageCache: {:X} not found, current size {}", id, table_.size());
      return std::nullopt;
    }
    return it->second.message;
  }

  bool MessageCache::insert(TopicMessage::Ptr message,
                            const MessageId &msg_id) {
    if (!message || msg_id.empty()) {
      return false;
    }
    auto it = table_.find(msg_id);
    if (it != table_.end()) {
      return false;
    }
    auto now = clock_();
    table_.emplace(msg_id, Record{now + message_lifetime_, std::move(message)});
    return true;
  }

  void MessageCache::shift() {
    if (table_.empty()) {
      return;
    }
    auto now = clock_();

    TRACE("MessageCache: size before shift: {}", table_.size());

    for (auto it = table_.begin(); it != table_.end();) {
      if (it->second.expires_at < now) {
        it = table_.erase(it);
      } else {
        ++it;
      }
    }

    TRACE("MessageCache: size after shift: {}", table_.size());
  }

}  // namespace libp2p::protocol::gossip
