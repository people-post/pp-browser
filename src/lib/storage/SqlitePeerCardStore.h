#pragma once

#include "lib/people/PeerCards.h"

#include <mutex>
#include <string>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** IPeerCardStore on profile.db (`peer_cards`, `peer_card_floors`; public material, plaintext). Thread-safe. */
class SqlitePeerCardStore : public IPeerCardStore {
public:
  explicit SqlitePeerCardStore(std::string profile_db_path);

  Roe<std::optional<PeerCardRecord>> Get(const std::string& account_id) const override;
  Roe<std::vector<PeerCardRecord>> List() const override;
  Roe<void> Put(const PeerCardRecord& record) override;
  Roe<int64_t> Floor(const std::string& account_id) const override;
  Roe<void> RaiseFloors(const std::vector<PeerCardFloor>& floors) override;
  Roe<std::vector<PeerCardFloor>> Floors() const override;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
