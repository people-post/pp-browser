/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/identify_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> IdentifyDeltaWire::encode() const {
  Writer w;
  for (const auto &proto : added_protocols) {
    w.writeStringField(1, proto);
  }
  for (const auto &proto : rm_protocols) {
    w.writeStringField(2, proto);
  }
  return w.take();
}

outcome::result<IdentifyDeltaWire> IdentifyDeltaWire::decode(BytesIn bytes) {
  IdentifyDeltaWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.added_protocols.emplace_back(value.begin(), value.end());
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.rm_protocols.emplace_back(value.begin(), value.end());
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

outcome::result<Bytes> IdentifyWire::encode() const {
  Writer w;
  if (public_key) {
    w.writeBytesField(1, *public_key);
  }
  for (const auto &addr : listen_addrs) {
    w.writeBytesField(2, addr);
  }
  for (const auto &proto : protocols) {
    w.writeStringField(3, proto);
  }
  if (observed_addr) {
    w.writeBytesField(4, *observed_addr);
  }
  if (protocol_version) {
    w.writeStringField(5, *protocol_version);
  }
  if (agent_version) {
    w.writeStringField(6, *agent_version);
  }
  if (delta) {
    OUTCOME_TRY(encoded_delta, delta->encode());
    w.writeSubmessageField(7, encoded_delta);
  }
  return w.take();
}

outcome::result<IdentifyWire> IdentifyWire::decode(BytesIn bytes) {
  IdentifyWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.public_key = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.listen_addrs.push_back(std::move(value));
        break;
      }
      case 3: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.protocols.emplace_back(value.begin(), value.end());
        break;
      }
      case 4: {
        OUTCOME_TRY(field_2, r.readLengthDelimited());
        out.observed_addr = std::move(field_2);
        break;
      }
      case 5: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.protocol_version = std::string(value.begin(), value.end());
        break;
      }
      case 6: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        out.agent_version = std::string(value.begin(), value.end());
        break;
      }
      case 7: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(decoded, IdentifyDeltaWire::decode(payload));
        out.delta = std::move(decoded);
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
