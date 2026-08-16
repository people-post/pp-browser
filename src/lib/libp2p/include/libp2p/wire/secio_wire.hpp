/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <optional>
#include <string>

#include <libp2p/common/types.hpp>
#include <libp2p/wire/wire_codec.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

struct SecioProposeWire {
  std::optional<Bytes> rand;
  std::optional<Bytes> pubkey;
  std::optional<std::string> exchanges;
  std::optional<std::string> ciphers;
  std::optional<std::string> hashes;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<SecioProposeWire> decode(BytesIn bytes);
};

struct SecioExchangeWire {
  std::optional<Bytes> epubkey;
  std::optional<Bytes> signature;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<SecioExchangeWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
