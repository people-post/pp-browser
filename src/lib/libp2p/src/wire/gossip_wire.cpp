/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/gossip_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> GossipSubOptsWire::encode() const {
  Writer w;
  if (subscribe) {
    w.writeBoolField(1, *subscribe);
  }
  if (topic_id) {
    w.writeStringField(2, *topic_id);
  }
  return w.take();
}

outcome::result<GossipSubOptsWire> GossipSubOptsWire::decode(BytesIn bytes) {
  GossipSubOptsWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(raw, r.readVarint());
        out.subscribe = raw != 0;
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.topic_id = std::string(value.begin(), value.end());
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipMessageWire::encode() const {
  Writer w;
  if (from) {
    w.writeBytesField(1, *from);
  }
  if (data) {
    w.writeBytesField(2, *data);
  }
  if (seqno) {
    w.writeBytesField(3, *seqno);
  }
  if (topic) {
    w.writeStringField(4, *topic);
  }
  if (signature) {
    w.writeBytesField(5, *signature);
  }
  if (key) {
    w.writeBytesField(6, *key);
  }
  return w.take();
}

outcome::result<GossipMessageWire> GossipMessageWire::decode(BytesIn bytes) {
  GossipMessageWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.from = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_2, r.readLengthDelimited());
        out.data = std::move(field_2);
        break;
      }
      case 3: {
        OUTCOME_TRY(field_3, r.readLengthDelimited());
        out.seqno = std::move(field_3);
        break;
      }
      case 4: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.topic = std::string(value.begin(), value.end());
        break;
      }
      case 5: {
        OUTCOME_TRY(field_4, r.readLengthDelimited());
        out.signature = std::move(field_4);
        break;
      }
      case 6: {
        OUTCOME_TRY(field_5, r.readLengthDelimited());
        out.key = std::move(field_5);
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipControlIHaveWire::encode() const {
  Writer w;
  if (topic_id) {
    w.writeStringField(1, *topic_id);
  }
  for (const auto &id : message_ids) {
    w.writeBytesField(2, id);
  }
  return w.take();
}

outcome::result<GossipControlIHaveWire> GossipControlIHaveWire::decode(
    BytesIn bytes) {
  GossipControlIHaveWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.topic_id = std::string(value.begin(), value.end());
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.message_ids.push_back(std::move(value));
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipControlIWantWire::encode() const {
  Writer w;
  for (const auto &id : message_ids) {
    w.writeBytesField(1, id);
  }
  return w.take();
}

outcome::result<GossipControlIWantWire> GossipControlIWantWire::decode(
    BytesIn bytes) {
  GossipControlIWantWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.message_ids.push_back(std::move(value));
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipControlGraftWire::encode() const {
  Writer w;
  if (topic_id) {
    w.writeStringField(1, *topic_id);
  }
  return w.take();
}

outcome::result<GossipControlGraftWire> GossipControlGraftWire::decode(
    BytesIn bytes) {
  GossipControlGraftWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.topic_id = std::string(value.begin(), value.end());
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipPeerInfoWire::encode() const {
  Writer w;
  if (peer_id) {
    w.writeBytesField(1, *peer_id);
  }
  if (signed_peer_record) {
    w.writeBytesField(2, *signed_peer_record);
  }
  return w.take();
}

outcome::result<GossipPeerInfoWire> GossipPeerInfoWire::decode(BytesIn bytes) {
  GossipPeerInfoWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_6, r.readLengthDelimited());
        out.peer_id = std::move(field_6);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_7, r.readLengthDelimited());
        out.signed_peer_record = std::move(field_7);
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipControlPruneWire::encode() const {
  Writer w;
  if (topic_id) {
    w.writeStringField(1, *topic_id);
  }
  for (const auto &peer : peers) {
    OUTCOME_TRY(encoded, peer.encode());
    w.writeSubmessageField(2, encoded);
  }
  if (backoff) {
    w.writeVarintField(3, *backoff);
  }
  return w.take();
}

outcome::result<GossipControlPruneWire> GossipControlPruneWire::decode(
    BytesIn bytes) {
  GossipControlPruneWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.topic_id = std::string(value.begin(), value.end());
        break;
      }
      case 2: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipPeerInfoWire::decode(payload));
        out.peers.push_back(std::move(decoded));
        break;
      }
      case 3: {
        OUTCOME_TRY(raw, r.readVarint());
        out.backoff = raw;
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

outcome::result<Bytes> GossipControlMessageWire::encode() const {
  Writer w;
  for (const auto &item : ihave) {
    OUTCOME_TRY(encoded, item.encode());
    w.writeSubmessageField(1, encoded);
  }
  for (const auto &item : iwant) {
    OUTCOME_TRY(encoded, item.encode());
    w.writeSubmessageField(2, encoded);
  }
  for (const auto &item : graft) {
    OUTCOME_TRY(encoded, item.encode());
    w.writeSubmessageField(3, encoded);
  }
  for (const auto &item : prune) {
    OUTCOME_TRY(encoded, item.encode());
    w.writeSubmessageField(4, encoded);
  }
  return w.take();
}

outcome::result<GossipControlMessageWire> GossipControlMessageWire::decode(
    BytesIn bytes) {
  GossipControlMessageWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipControlIHaveWire::decode(payload));
        out.ihave.push_back(std::move(decoded));
        break;
      }
      case 2: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipControlIWantWire::decode(payload));
        out.iwant.push_back(std::move(decoded));
        break;
      }
      case 3: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipControlGraftWire::decode(payload));
        out.graft.push_back(std::move(decoded));
        break;
      }
      case 4: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipControlPruneWire::decode(payload));
        out.prune.push_back(std::move(decoded));
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

void GossipRpcWire::clear() {
  subscriptions.clear();
  publish.clear();
  control.reset();
}

outcome::result<Bytes> GossipRpcWire::encode() const {
  Writer w;
  for (const auto &sub : subscriptions) {
    OUTCOME_TRY(encoded, sub.encode());
    w.writeSubmessageField(1, encoded);
  }
  for (const auto &msg : publish) {
    OUTCOME_TRY(encoded, msg.encode());
    w.writeSubmessageField(2, encoded);
  }
  if (control) {
    OUTCOME_TRY(encoded, control->encode());
    w.writeSubmessageField(3, encoded);
  }
  return w.take();
}

outcome::result<GossipRpcWire> GossipRpcWire::decode(BytesIn bytes) {
  GossipRpcWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipSubOptsWire::decode(payload));
        out.subscriptions.push_back(std::move(decoded));
        break;
      }
      case 2: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipMessageWire::decode(payload));
        out.publish.push_back(std::move(decoded));
        break;
      }
      case 3: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, GossipControlMessageWire::decode(payload));
        out.control = std::move(decoded);
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  return out;
}

}  // namespace libp2p::wire
