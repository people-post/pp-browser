/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>

#include "common.hpp"

namespace libp2p::protocol::gossip {

  /// Message cache with expiration
  class MessageCache {
   public:
    /// External time function
    using TimeFunction = std::function<Time()>;

    MessageCache(Time message_lifetime, TimeFunction clock);

    ~MessageCache();

    bool contains(const MessageId &id) const;

    /// Returns message by id if found
    std::optional<TopicMessage::Ptr> getMessage(const MessageId &id) const;

    /// Inserts a new message into cache. If already there, returns false
    bool insert(TopicMessage::Ptr message, const MessageId &msg_id);

    /// Purges expired messages and updates seen notification data
    void shift();

   private:
    struct Record {
      Time expires_at;
      TopicMessage::Ptr message;
    };

    const Time message_lifetime_;
    TimeFunction clock_;
    std::unordered_map<MessageId, Record> table_;
  };

}  // namespace libp2p::protocol::gossip
