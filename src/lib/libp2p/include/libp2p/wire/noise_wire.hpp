/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <string>
#include <vector>

#include <libp2p/common/types.hpp>
#include <libp2p/wire/wire_codec.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

struct NoiseHandshakePayloadWire {
  Bytes identity_key;
  Bytes identity_sig;
  Bytes data;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<NoiseHandshakePayloadWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
