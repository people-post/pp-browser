/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include "message_builder.hpp"

#include <libp2p/multi/uvarint.hpp>
#include <libp2p/wire/gossip_wire.hpp>

namespace libp2p::protocol::gossip {

  MessageBuilder::MessageBuilder() : empty_(true), control_not_empty_(false) {}

  MessageBuilder::~MessageBuilder() = default;

  void MessageBuilder::clear() {
    wire_msg_.clear();
    empty_ = true;
    control_not_empty_ = false;
    ihaves_.clear();
    iwant_.clear();
  }

  void MessageBuilder::reset() {
    wire_msg_ = wire::GossipRpcWire{};
    empty_ = true;
    control_not_empty_ = false;
    decltype(ihaves_){}.swap(ihaves_);
    decltype(iwant_){}.swap(iwant_);
    decltype(messages_added_){}.swap(messages_added_);
  }

  void MessageBuilder::ensureWireMessage() {
    if (!wire_msg_.control && control_not_empty_) {
      wire_msg_.control.emplace();
    }
  }

  bool MessageBuilder::empty() const {
    return empty_;
  }

  outcome::result<SharedBuffer> MessageBuilder::serialize() {
    ensureWireMessage();
    if (wire_msg_.control) {
      auto &control = *wire_msg_.control;
      control.ihave.clear();
      control.iwant.clear();

      for (auto &[topic, message_ids] : ihaves_) {
        wire::GossipControlIHaveWire ih;
        ih.topic_id = topic;
        ih.message_ids = message_ids;
        control.ihave.push_back(std::move(ih));
      }

      if (!iwant_.empty()) {
        wire::GossipControlIWantWire iw;
        iw.message_ids = iwant_;
        control.iwant.push_back(std::move(iw));
      }
    }

    auto encoded_res = wire_msg_.encode();
    if (!encoded_res) {
      return Error::MESSAGE_SERIALIZE_ERROR;
    }
    const auto &encoded = encoded_res.value();
    size_t msg_sz = encoded.size();

    auto varint_len = multi::UVarint{msg_sz};
    auto varint_vec = varint_len.toVector();
    size_t prefix_sz = varint_vec.size();

    auto buffer = std::make_shared<Bytes>();
    buffer->resize(prefix_sz + msg_sz);
    memcpy(buffer->data(), varint_vec.data(), prefix_sz);
    memcpy(buffer->data() + prefix_sz, encoded.data(), msg_sz);

    static constexpr size_t kSizeThreshold = 8192;
    if (msg_sz > kSizeThreshold) {
      reset();
    } else {
      clear();
    }

    return buffer;
  }

  void MessageBuilder::addSubscription(bool subscribe, const TopicId &topic) {
    wire::GossipSubOptsWire sub;
    sub.subscribe = subscribe;
    sub.topic_id = topic;
    wire_msg_.subscriptions.push_back(std::move(sub));
    empty_ = false;
  }

  void MessageBuilder::addIHave(const TopicId &topic, const MessageId &msg_id) {
    ihaves_[topic].push_back(msg_id);
    control_not_empty_ = true;
    empty_ = false;
  }

  void MessageBuilder::addIWant(const MessageId &msg_id) {
    iwant_.push_back(msg_id);
    control_not_empty_ = true;
    empty_ = false;
  }

  void MessageBuilder::addGraft(const TopicId &topic) {
    ensureWireMessage();
    wire::GossipControlGraftWire graft;
    graft.topic_id = topic;
    wire_msg_.control->graft.push_back(std::move(graft));
    control_not_empty_ = true;
    empty_ = false;
  }

  void MessageBuilder::addPrune(const TopicId &topic) {
    ensureWireMessage();
    wire::GossipControlPruneWire prune;
    prune.topic_id = topic;
    wire_msg_.control->prune.push_back(std::move(prune));
    control_not_empty_ = true;
    empty_ = false;
  }

  void MessageBuilder::addMessage(const TopicMessage &msg,
                                  const MessageId &msg_id) {
    if (messages_added_.count(msg_id) != 0) {
      return;
    }
    messages_added_.insert(msg_id);

    wire::GossipMessageWire dst;
    dst.from = msg.from;
    dst.data = msg.data;
    dst.seqno = msg.seq_no;
    dst.topic = msg.topic;
    if (msg.signature) {
      dst.signature = msg.signature.value();
    }
    if (msg.key) {
      dst.key = msg.key.value();
    }
    wire_msg_.publish.push_back(std::move(dst));
    empty_ = false;
  }

  outcome::result<Bytes> MessageBuilder::signableMessage(
      const TopicMessage &msg) {
    wire::GossipMessageWire wire_msg;
    wire_msg.from = msg.from;
    wire_msg.data = msg.data;
    wire_msg.seqno = msg.seq_no;
    wire_msg.topic = msg.topic;
    auto encoded_res = wire_msg.encode();
    if (!encoded_res) {
      return Error::MESSAGE_SERIALIZE_ERROR;
    }
    constexpr std::string_view kPrefix{"libp2p-pubsub:"};
    const auto &encoded = encoded_res.value();
    Bytes signable;
    signable.resize(kPrefix.size() + encoded.size());
    std::copy(kPrefix.begin(), kPrefix.end(), signable.begin());
    std::copy(encoded.begin(), encoded.end(), signable.begin() + kPrefix.size());
    return signable;
  }
}  // namespace libp2p::protocol::gossip
