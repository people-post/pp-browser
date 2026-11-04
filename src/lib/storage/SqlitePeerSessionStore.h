#pragma once

#include "foundation/crypto/IDekConsumer.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "foundation/crypto/CryptoTypes.h"

#include "common/Module.h"

#include <mutex>
#include <string>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * PSK persistence on profile.db, PSK columns encrypted with the profile DEK: account-level sessions on
 * `chat_targets` (shared with the thread's numbering), device-pair sessions (M021) on `device_sessions`,
 * which hold their own numbering.
 */
class SqlitePeerSessionStore : public Module, public IPeerSessionStore, public IDekConsumer {
public:
  explicit SqlitePeerSessionStore(std::string profile_db_path, std::string profile_id = {});
  ~SqlitePeerSessionStore();

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  Roe<std::optional<PeerSessionRecord>> Load(const PeerSessionKey& key) const override;
  Roe<std::vector<PeerSessionRecord>> List() const override;
  Roe<void> Save(const PeerSessionRecord& record) override;
  Roe<uint64_t> AllocateSenderSeq(const PeerSessionKey& key) override;

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> SaveDeviceSession(sqlite3* db, const PeerSessionRecord& encrypted, const std::string& retired_json);
  Roe<void> RequireDek() const;
  Roe<std::string> EncryptPskB64(const std::string& plaintext_b64) const;
  Roe<std::string> DecryptPskB64(const std::string& ciphertext_b64) const;
  Roe<std::string> EncryptFieldB64(const std::string& plaintext_b64, const char* aad_kind) const;
  Roe<std::string> DecryptFieldB64(const std::string& ciphertext_b64, const char* aad_kind) const;
  Roe<PeerSessionRecord> DecryptRecord(PeerSessionRecord record) const;
  Roe<PeerSessionRecord> EncryptRecord(PeerSessionRecord record) const;

  std::string profile_db_path_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
  mutable sqlite3* db_ = nullptr;
};

} // namespace pbr
