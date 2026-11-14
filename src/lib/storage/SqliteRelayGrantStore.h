#pragma once

#include "lib/delivery/IRelayGrantStore.h"

#include <mutex>
#include <string>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * IRelayGrantStore on profile.db (`relay_grants`, multi-relay MR005): one row per peer and direction — the grant a
 * peer gave us (`received`), the one we gave it (`issued`, with whether it confirmed it). Plaintext: a grant is a
 * signed statement for the relay, which sees it on every send.
 */
class SqliteRelayGrantStore : public IRelayGrantStore {
public:
  explicit SqliteRelayGrantStore(std::string profile_db_path);

  Roe<std::optional<RelayGrant>> Received(const std::string& recipient_account_id) override;
  Roe<void> PutReceived(const RelayGrant& grant) override;
  Roe<void> DropReceived(const std::string& recipient_account_id) override;
  Roe<std::optional<Issued>> IssuedTo(const std::string& sender_account_id) override;
  Roe<void> PutIssued(const Issued& issued) override;
  Roe<void> DropIssued(const std::string& sender_account_id) override;

private:
  Roe<sqlite3*> OpenDb() const;
  /** The row for (`direction`, `peer`): its grant and confirmed flag. */
  Roe<std::optional<Issued>> Get(const char* direction, const std::string& peer);
  Roe<void> Put(const char* direction, const std::string& peer, const RelayGrant& grant, bool confirmed,
                bool only_if_newer);
  Roe<void> Drop(const char* direction, const std::string& peer);

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable bool schema_ready_ = false;
};

} // namespace pbr
