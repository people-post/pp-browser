#pragma once

#include "base/crypto/IPskSessionStore.h"

#include "common/Module.h"

#include <mutex>
#include <string>

struct sqlite3;

namespace pbr {

/** v1 PSK persistence on profile.db chat_targets (E008/D084). */
class SqlitePskSessionStore : public Module, public IPskSessionStore {
public:
  explicit SqlitePskSessionStore(std::string profile_db_path);

  Roe<std::optional<PskSessionRecord>> Load(const ChatTargetKey& key) const override;
  Roe<void> Save(const PskSessionRecord& record) override;
  Roe<ByteVector> GenerateMasterPsk() override;
  Roe<std::optional<std::string>> ResolveMasterPskForEpoch(const ChatTargetKey& key,
                                                           uint32_t envelope_epoch) const override;
  Roe<void> MarkPskVerified(const ChatTargetKey& key, int64_t verified_at_ms) override;
  Roe<bool> IsPskVerified(const ChatTargetKey& key) const override;
  Roe<PskBundleV1> ExportPskBundle(const ChatTargetKey& key) const override;
  Roe<void> ImportPskBundle(const ChatTargetKey& key, const PskBundleV1& bundle) override;

private:
  Roe<sqlite3*> OpenDb() const;

  std::string profile_db_path_;
  mutable std::mutex mutex_;
  mutable sqlite3* db_ = nullptr;
};

} // namespace pbr
