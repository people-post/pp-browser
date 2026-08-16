/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/plaintext_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> PlaintextExchangeWire::encode() const {
  Writer w;
  if (id) {
    w.writeBytesField(1, *id);
  }
  if (pubkey) {
    OUTCOME_TRY(encoded_key, pubkey->encode());
    w.writeSubmessageField(2, encoded_key);
  }
  return w.take();
}

outcome::result<PlaintextExchangeWire> PlaintextExchangeWire::decode(
    BytesIn bytes) {
  PlaintextExchangeWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.id = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(payload, r.readLengthDelimited());
        OUTCOME_TRY(key, PublicKeyWire::decode(payload));
        out.pubkey = std::move(key);
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
