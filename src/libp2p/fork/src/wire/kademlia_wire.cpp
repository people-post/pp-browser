/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/kademlia_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> KademliaRecordWire::encode() const {
  Writer w;
  w.writeBytesField(1, key);
  w.writeBytesField(2, value);
  w.writeStringField(5, time_received);
  return w.take();
}

outcome::result<KademliaRecordWire> KademliaRecordWire::decode(BytesIn bytes) {
  KademliaRecordWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.key = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_2, r.readLengthDelimited());
        out.value = std::move(field_2);
        break;
      }
      case 5: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.time_received = std::string(value.begin(), value.end());
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

outcome::result<Bytes> KademliaPeerWire::encode() const {
  Writer w;
  w.writeBytesField(1, id);
  for (const auto &addr : addrs) {
    w.writeBytesField(2, addr);
  }
  w.writeVarintField(3, static_cast<uint64_t>(connection));
  return w.take();
}

outcome::result<KademliaPeerWire> KademliaPeerWire::decode(BytesIn bytes) {
  KademliaPeerWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_3, r.readLengthDelimited());
        out.id = std::move(field_3);
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.addrs.push_back(std::move(value));
        break;
      }
      case 3: {
        OUTCOME_TRY(raw, r.readVarint());
        out.connection = static_cast<KademliaConnectionTypeWire>(raw);
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

outcome::result<Bytes> KademliaMessageWire::encode() const {
  Writer w;
  w.writeVarintField(1, static_cast<uint64_t>(type));
  w.writeBytesField(2, key);
  if (record) {
    OUTCOME_TRY(encoded_record, record->encode());
    w.writeSubmessageField(3, encoded_record);
  }
  for (const auto &peer : closer_peers) {
    OUTCOME_TRY(encoded_peer, peer.encode());
    w.writeSubmessageField(8, encoded_peer);
  }
  for (const auto &peer : provider_peers) {
    OUTCOME_TRY(encoded_peer, peer.encode());
    w.writeSubmessageField(9, encoded_peer);
  }
  return w.take();
}

outcome::result<KademliaMessageWire> KademliaMessageWire::decode(BytesIn bytes) {
  KademliaMessageWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(raw, r.readVarint());
        out.type = static_cast<KademliaMessageTypeWire>(raw);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_4, r.readLengthDelimited());
        out.key = std::move(field_4);
        break;
      }
      case 3: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, KademliaRecordWire::decode(payload));
        out.record = std::move(decoded);
        break;
      }
      case 8: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, KademliaPeerWire::decode(payload));
        out.closer_peers.push_back(std::move(decoded));
        break;
      }
      case 9: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, KademliaPeerWire::decode(payload));
        out.provider_peers.push_back(std::move(decoded));
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
