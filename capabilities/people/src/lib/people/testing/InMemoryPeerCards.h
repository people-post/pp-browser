#pragma once

// People's card store in memory, for its tests and the tests above it (CR024). Never linked into the app or pp-node.

#include "lib/people/PeerCards.h"

#include <map>
#include <mutex>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** IPeerCardStore in memory: one record per account, floors that only rise (as the SQLite one). */
class InMemoryPeerCardStore : public IPeerCardStore {
public:
  Roe<std::optional<PeerCardRecord>> Get(const std::string& account_id) const override;
  Roe<std::vector<PeerCardRecord>> List() const override;
  Roe<void> Put(const PeerCardRecord& record) override;
  Roe<int64_t> Floor(const std::string& account_id) const override;
  Roe<void> RaiseFloors(const std::vector<PeerCardFloor>& floors) override;
  Roe<std::vector<PeerCardFloor>> Floors() const override;

private:
  mutable std::mutex mutex_;
  std::map<std::string, PeerCardRecord> records_;
  std::map<std::string, int64_t> floors_;
};

namespace detail {
struct InMemoryPeerCardRows {
  InMemoryPeerCardStore rows;
};
} // namespace detail

/** PeerCards over its own in-memory store: what a test holds where the app holds PeerCards over storage. */
class InMemoryPeerCards : private detail::InMemoryPeerCardRows, public PeerCards {
public:
  InMemoryPeerCards() : PeerCards(rows) {}
};

} // namespace pbr
