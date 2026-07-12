#pragma once

#include "base/crypto/IDekConsumer.h"
#include "base/crypto/IPskSessionStore.h"
#include "base/crypto/CryptoTypes.h"

#include "common/Module.h"

#include <mutex>
#include <string>

struct sqlite3;

namespace pbr {

/** v1 PSK persistence on profile.db chat_targets; PSK columns encrypted with profile DEK. */
class SqlitePskSessionStore : public Module, public IPskSessionStore, public IDekConsumer {
public:
  explicit SqlitePskSessionStore(std::string profile_db_path, std::string profile_id = {});

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

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
  Roe<void> RequireDek() const;
  Roe<std::string> EncryptPskB64(const std::string& plaintext_b64) const;
  Roe<std::string> DecryptPskB64(const std::string& ciphertext_b64) const;
  Roe<PskSessionRecord> DecryptRecord(PskSessionRecord record) const;
  Roe<PskSessionRecord> EncryptRecord(PskSessionRecord record) const;

  std::string profile_db_path_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
  mutable sqlite3* db_ = nullptr;
};

} // namespace pbr
