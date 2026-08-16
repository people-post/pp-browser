/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <libp2p/common/types.hpp>
#include <libp2p/wire/wire_codec.hpp>
#include <libp2p/outcome/outcome.hpp>

namespace libp2p::wire {

enum class KademliaMessageTypeWire : int32_t {
  kPutValue = 0,
  kGetValue = 1,
  kAddProvider = 2,
  kGetProviders = 3,
  kFindNode = 4,
  kPing = 5,
};

enum class KademliaConnectionTypeWire : int32_t {
  kNotConnected = 0,
  kConnected = 1,
  kCanConnect = 2,
  kCannotConnect = 3,
};

struct KademliaRecordWire {
  Bytes key;
  Bytes value;
  std::string time_received;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<KademliaRecordWire> decode(BytesIn bytes);
};

struct KademliaPeerWire {
  Bytes id;
  std::vector<Bytes> addrs;
  KademliaConnectionTypeWire connection = KademliaConnectionTypeWire::kNotConnected;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<KademliaPeerWire> decode(BytesIn bytes);
};

struct KademliaMessageWire {
  KademliaMessageTypeWire type = KademliaMessageTypeWire::kPing;
  Bytes key;
  std::optional<KademliaRecordWire> record;
  std::vector<KademliaPeerWire> closer_peers;
  std::vector<KademliaPeerWire> provider_peers;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<KademliaMessageWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
