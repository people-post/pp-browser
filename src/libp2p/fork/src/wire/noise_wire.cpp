/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <libp2p/wire/noise_wire.hpp>

namespace libp2p::wire {

outcome::result<Bytes> NoiseHandshakePayloadWire::encode() const {
  Writer w;
  w.writeBytesField(1, identity_key);
  w.writeBytesField(2, identity_sig);
  w.writeBytesField(3, data);
  return w.take();
}

outcome::result<NoiseHandshakePayloadWire> NoiseHandshakePayloadWire::decode(
    BytesIn bytes) {
  NoiseHandshakePayloadWire out;
  Reader r(bytes);
  while (!r.eof()) {
    OUTCOME_TRY(tag, r.readTag());
    switch (tag.number) {
      case 1: {
        OUTCOME_TRY(field_1, r.readLengthDelimited());
        out.identity_key = std::move(field_1);
        break;
      }
      case 2: {
        OUTCOME_TRY(field_2, r.readLengthDelimited());
        out.identity_sig = std::move(field_2);
        break;
      }
      case 3: {
        OUTCOME_TRY(field_3, r.readLengthDelimited());
        out.data = std::move(field_3);
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
