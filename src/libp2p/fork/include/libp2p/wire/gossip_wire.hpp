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

struct GossipSubOptsWire {
  std::optional<bool> subscribe;
  std::optional<std::string> topic_id;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipSubOptsWire> decode(BytesIn bytes);
};

struct GossipMessageWire {
  std::optional<Bytes> from;
  std::optional<Bytes> data;
  std::optional<Bytes> seqno;
  std::optional<std::string> topic;
  std::optional<Bytes> signature;
  std::optional<Bytes> key;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipMessageWire> decode(BytesIn bytes);
};

struct GossipControlIHaveWire {
  std::optional<std::string> topic_id;
  std::vector<Bytes> message_ids;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipControlIHaveWire> decode(BytesIn bytes);
};

struct GossipControlIWantWire {
  std::vector<Bytes> message_ids;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipControlIWantWire> decode(BytesIn bytes);
};

struct GossipControlGraftWire {
  std::optional<std::string> topic_id;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipControlGraftWire> decode(BytesIn bytes);
};

struct GossipPeerInfoWire {
  std::optional<Bytes> peer_id;
  std::optional<Bytes> signed_peer_record;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipPeerInfoWire> decode(BytesIn bytes);
};

struct GossipControlPruneWire {
  std::optional<std::string> topic_id;
  std::vector<GossipPeerInfoWire> peers;
  std::optional<uint64_t> backoff;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipControlPruneWire> decode(BytesIn bytes);
};

struct GossipControlMessageWire {
  std::vector<GossipControlIHaveWire> ihave;
  std::vector<GossipControlIWantWire> iwant;
  std::vector<GossipControlGraftWire> graft;
  std::vector<GossipControlPruneWire> prune;

  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipControlMessageWire> decode(BytesIn bytes);
};

struct GossipRpcWire {
  std::vector<GossipSubOptsWire> subscriptions;
  std::vector<GossipMessageWire> publish;
  std::optional<GossipControlMessageWire> control;

  void clear();
  [[nodiscard]] outcome::result<Bytes> encode() const;
  static outcome::result<GossipRpcWire> decode(BytesIn bytes);
};

}  // namespace libp2p::wire
