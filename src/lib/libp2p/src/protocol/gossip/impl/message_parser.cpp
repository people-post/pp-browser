/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include "message_parser.hpp"

#include <libp2p/log/logger.hpp>
#include <libp2p/wire/gossip_wire.hpp>

#include "message_receiver.hpp"

namespace libp2p::protocol::gossip {

  namespace {
    auto log() {
      static auto logger = log::createLogger("gossip");
      return logger.get();
    }
  }  // namespace

  MessageParser::MessageParser() = default;
  MessageParser::~MessageParser() = default;

  bool MessageParser::parse(BytesIn bytes) {
    wire_msg_.clear();
    auto decoded = wire::GossipRpcWire::decode(bytes);
    if (!decoded) {
      return false;
    }
    wire_msg_ = std::move(decoded.value());
    return true;
  }

  void MessageParser::dispatch(const PeerContextPtr &from,
                               MessageReceiver &receiver) {
    for (const auto &s : wire_msg_.subscriptions) {
      if (!s.subscribe || !s.topic_id) {
        continue;
      }
      receiver.onSubscription(from, *s.subscribe, *s.topic_id);
    }

    if (wire_msg_.control) {
      const auto &c = *wire_msg_.control;

      for (const auto &h : c.ihave) {
        if (!h.topic_id || h.message_ids.empty()) {
          continue;
        }
        const TopicId &topic = *h.topic_id;
        for (const auto &msg_id : h.message_ids) {
          if (msg_id.empty()) {
            continue;
          }
          receiver.onIHave(from, topic, msg_id);
        }
      }

      for (const auto &w : c.iwant) {
        if (w.message_ids.empty()) {
          continue;
        }
        for (const auto &msg_id : w.message_ids) {
          if (msg_id.empty()) {
            continue;
          }
          receiver.onIWant(from, msg_id);
        }
      }

      for (const auto &gr : c.graft) {
        if (!gr.topic_id) {
          continue;
        }
        receiver.onGraft(from, *gr.topic_id);
      }

      for (const auto &pr : c.prune) {
        if (!pr.topic_id) {
          continue;
        }
        uint64_t backoff_time = pr.backoff.value_or(60);
        log()->debug(
            "prune backoff={}, {} peers", backoff_time, pr.peers.size());
        for (const auto &peer : pr.peers) {
          log()->debug("peer id size={}, signed peer record size={}",
                       peer.peer_id ? peer.peer_id->size() : 0,
                       peer.signed_peer_record ? peer.signed_peer_record->size()
                                               : 0);
        }
        receiver.onPrune(from, *pr.topic_id, backoff_time);
      }
    }

    for (const auto &m : wire_msg_.publish) {
      if (!m.from || !m.data || !m.seqno || !m.topic) {
        continue;
      }
      auto message = std::make_shared<TopicMessage>(
          *m.from, *m.seqno, *m.data);
      message->topic = *m.topic;
      if (m.signature) {
        message->signature = *m.signature;
      }
      if (m.key) {
        message->key = *m.key;
      }
      receiver.onTopicMessage(from, std::move(message));
    }

    receiver.onMessageEnd(from);
  }

}  // namespace libp2p::protocol::gossip
