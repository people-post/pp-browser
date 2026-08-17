/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/keys_wire.hpp>

namespace libp2p::wire {

namespace {

void encodeKeyBody(Writer &w, KeyTypeWire type, BytesIn data) {
  w.writeTag(1, WireType::kVarint);
  w.writeVarint(static_cast<uint64_t>(type));
  w.writeBytesField(2, data);
}

outcome::result<std::pair<KeyTypeWire, Bytes>> decodeKeyBody(BytesIn bytes) {
  Reader r(bytes);
  std::optional<KeyTypeWire> type;
  std::optional<Bytes> data;
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(raw, r.readVarint());
        type = static_cast<KeyTypeWire>(raw);
        break;
      }
      case 2: {
        OUTCOME_TRY(value, r.readLengthDelimited());
        data = std::move(value);
        break;
      }
      default: {
        OUTCOME_TRY(r.skipField(tag.type));
        break;
      }
    }
  }
  if (!type || !data) {
    return DecodeError::MISSING_REQUIRED_FIELD;
  }
  return std::make_pair(*type, std::move(*data));
}

}  // namespace

outcome::result<Bytes> PublicKeyWire::encode() const {
  Writer w;
  encodeKeyBody(w, type, data);
  return w.take();
}

outcome::result<PublicKeyWire> PublicKeyWire::decode(BytesIn bytes) {
  OUTCOME_TRY(body, decodeKeyBody(bytes));
  PublicKeyWire out;
  out.type = body.first;
  out.data = std::move(body.second);
  return out;
}

outcome::result<Bytes> PrivateKeyWire::encode() const {
  Writer w;
  encodeKeyBody(w, type, data);
  return w.take();
}

outcome::result<PrivateKeyWire> PrivateKeyWire::decode(BytesIn bytes) {
  OUTCOME_TRY(body, decodeKeyBody(bytes));
  PrivateKeyWire out;
  out.type = body.first;
  out.data = std::move(body.second);
  return out;
}

}  // namespace libp2p::wire
