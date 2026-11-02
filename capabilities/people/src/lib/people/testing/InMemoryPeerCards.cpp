#include "lib/people/testing/InMemoryPeerCards.h"

#include <algorithm>

namespace pbr {

Roe<std::optional<PeerCardRecord>> InMemoryPeerCardStore::Get(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto record = records_.find(account_id);
  return record == records_.end() ? std::optional<PeerCardRecord>() : std::optional<PeerCardRecord>(record->second);
}

Roe<std::vector<PeerCardRecord>> InMemoryPeerCardStore::List() const {
  std::lock_guard lock(mutex_);
  std::vector<PeerCardRecord> records;
  for (const auto& [account_id, record] : records_) {
    records.push_back(record);
  }
  return records;
}

Roe<void> InMemoryPeerCardStore::Put(const PeerCardRecord& record) {
  std::lock_guard lock(mutex_);
  records_[record.keys.account_id] = record;
  return {};
}

Roe<int64_t> InMemoryPeerCardStore::Floor(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto floor = floors_.find(account_id);
  return floor == floors_.end() ? int64_t{0} : floor->second;
}

Roe<void> InMemoryPeerCardStore::RaiseFloors(const std::vector<PeerCardFloor>& floors) {
  std::lock_guard lock(mutex_);
  for (const PeerCardFloor& floor : floors) {
    int64_t& kept = floors_[floor.account_id];
    kept = std::max(kept, floor.issued_at);
  }
  return {};
}

Roe<std::vector<PeerCardFloor>> InMemoryPeerCardStore::Floors() const {
  std::lock_guard lock(mutex_);
  std::vector<PeerCardFloor> floors;
  for (const auto& [account_id, issued_at] : floors_) {
    floors.push_back({account_id, issued_at});
  }
  return floors;
}

} // namespace pbr
