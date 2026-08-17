/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <libp2p/common/types.hpp>
#include <libp2p/wire/wire_codec.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

enum class KeyTypeWire : int32_t {
  kRsa = 0,
  kEd25519 = 1,
  kSecp256k1 = 2,
  kEcdsa = 3,
  /// Provisional pp-browser wire code (libp2p-pq-transport P003).
  kMlDsa65 = 4,
};

struct PublicKeyWire {
  KeyTypeWire type = KeyTypeWire::kRsa;
  Bytes data;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<PublicKeyWire> decode(BytesIn bytes);
};

struct PrivateKeyWire {
  KeyTypeWire type = KeyTypeWire::kRsa;
  Bytes data;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<PrivateKeyWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
