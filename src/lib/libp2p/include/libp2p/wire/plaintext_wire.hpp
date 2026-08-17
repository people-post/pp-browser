/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>

#include <libp2p/common/types.hpp>
#include <libp2p/wire/keys_wire.hpp>
#include <libp2p/wire/wire_codec.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

struct PlaintextExchangeWire {
  std::optional<Bytes> id;
  std::optional<PublicKeyWire> pubkey;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<PlaintextExchangeWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
