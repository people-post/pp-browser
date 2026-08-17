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

struct IdentifyDeltaWire {
  std::vector<std::string> added_protocols;
  std::vector<std::string> rm_protocols;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<IdentifyDeltaWire> decode(BytesIn bytes);
};

struct IdentifyWire {
  std::optional<std::string> protocol_version;
  std::optional<std::string> agent_version;
  std::optional<Bytes> public_key;
  std::vector<Bytes> listen_addrs;
  std::optional<Bytes> observed_addr;
  std::vector<std::string> protocols;
  std::optional<IdentifyDeltaWire> delta;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<IdentifyWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
