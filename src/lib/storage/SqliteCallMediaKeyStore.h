#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "lib/calls/ICallMediaKeyStore.h"

#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/** ICallMediaKeyStore on profile.db, the keys encrypted with the profile DEK (V011). */
class SqliteCallMediaKeyStore : public Module, public ICallMediaKeyStore, public IDekConsumer {
public:
  explicit SqliteCallMediaKeyStore(std::string profile_db_path, std::string profile_id = {});

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  Roe<std::string> PutEpochKey(const std::string& call_id, uint32_t media_epoch, const ByteVector& key_bytes) override;
  Roe<std::optional<ByteVector>> LoadEpochKey(const std::string& call_id, uint32_t media_epoch) const override;

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> RequireDek() const;

  std::string profile_db_path_;
  std::string profile_id_;
  /** The call schema runs once per store, not on every short-lived connection. */
  mutable std::mutex schema_mutex_;
  mutable bool schema_ready_ = false;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
};

} // namespace pbr
