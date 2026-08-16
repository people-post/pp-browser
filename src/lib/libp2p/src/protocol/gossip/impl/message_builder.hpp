/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <map>
#include <unordered_set>

#include "common.hpp"

#include <libp2p/wire/gossip_wire.hpp>

namespace libp2p::protocol::gossip {

  class MessageBuilder {
   public:
    MessageBuilder(MessageBuilder &&) = default;
    MessageBuilder &operator=(MessageBuilder &&) = default;
    MessageBuilder(const MessageBuilder &) = delete;
    MessageBuilder &operator=(const MessageBuilder &) = delete;

    MessageBuilder();

    ~MessageBuilder();

    void reset();

    bool empty() const;

    outcome::result<SharedBuffer> serialize();

    void addSubscription(bool subscribe, const TopicId &topic);

    void addIHave(const TopicId &topic, const MessageId &msg_id);

    void addIWant(const MessageId &msg_id);

    void addGraft(const TopicId &topic);

    void addPrune(const TopicId &topic);

    void addMessage(const TopicMessage &msg, const MessageId &msg_id);

    static outcome::result<Bytes> signableMessage(const TopicMessage &msg);

   private:
    void ensureWireMessage();

    void clear();

    wire::GossipRpcWire wire_msg_;
    bool empty_;
    bool control_not_empty_;

    std::map<TopicId, std::vector<MessageId>> ihaves_;
    std::vector<MessageId> iwant_;
    std::unordered_set<MessageId> messages_added_;
  };
}  // namespace libp2p::protocol::gossip
