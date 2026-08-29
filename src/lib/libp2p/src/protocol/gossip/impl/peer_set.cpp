/**
 * Copyright Quadrivium LLC
 * All Rights Reserved
 * SPDX-License-Identifier: Apache-2.0
 */

#include <optional>
#include "peer_set.hpp"

#include <algorithm>
#include <chrono>
#include <random>

namespace libp2p::protocol::gossip {

  std::optional<PeerContextPtr> PeerSet::find(const peer::PeerId &id) const {
    auto it = peers_.find(id);
    if (it == peers_.end()) {
      return std::nullopt;
    }
    return *it;
  }

  bool PeerSet::contains(const peer::PeerId &id) const {
    return peers_.count(id) != 0;
  }

  bool PeerSet::insert(PeerContextPtr ctx) {
    if (!ctx || peers_.find(ctx) != peers_.end()) {
      return false;
    }
    peers_.emplace(std::move(ctx));
    return true;
  }

  std::optional<PeerContextPtr> PeerSet::erase(const peer::PeerId &id) {
    auto it = peers_.find(id);
    if (it == peers_.end()) {
      return std::nullopt;
    }
    std::optional<PeerContextPtr> ret(*it);
    peers_.erase(it);
    return ret;
  }

  void PeerSet::clear() {
    peers_.clear();
  }

  bool PeerSet::empty() const {
    return peers_.empty();
  }

  size_t PeerSet::size() const {
    return peers_.size();
  }

  std::vector<PeerContextPtr> PeerSet::selectRandomPeers(size_t n) const {
    std::vector<PeerContextPtr> ret;
    if (n > 0 && !empty()) {
      ret.reserve(n > size() ? size() : n);
      std::mt19937 gen;
      gen.seed(std::chrono::system_clock::now().time_since_epoch().count());
      std::sample(
          peers_.begin(), peers_.end(), std::back_inserter(ret), n, gen);
    }
    return ret;
  }

  void PeerSet::selectAll(const SelectCallback &callback) const {
    std::for_each(peers_.begin(), peers_.end(), callback);
  }

  void PeerSet::selectIf(const SelectCallback &callback,
                         const FilterCallback &filter) const {
    for (const auto &peer : peers_) {
      if (filter(peer)) {
        callback(peer);
      }
    }
  }

  void PeerSet::eraseIf(const FilterCallback &filter) {
    for (auto it = peers_.begin(); it != peers_.end();) {
      if (filter(*it)) {
        it = peers_.erase(it);
      } else {
        ++it;
      }
    }
  }

}  // namespace libp2p::protocol::gossip
