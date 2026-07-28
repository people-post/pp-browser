#pragma once

#include "base/crypto/CryptoTypes.h"
#include "base/crypto/IDekConsumer.h"

#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace pbr {

/**
 * Vault-backed call media key slots (V011).
 * Encrypts epoch key bytes under the profile DEK; wrap-to-peer is stubbed until a2.
 */
class CallMediaKeyStore : public Module, public IDekConsumer {
public:
  explicit CallMediaKeyStore(std::string profile_db_path, std::string profile_id = {});

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  /** Persist plaintext key bytes for (call_id, epoch); returns opaque media_key_id. */
  Roe<std::string> PutEpochKey(const std::string& call_id, uint32_t media_epoch, const ByteVector& key_bytes);
  Roe<std::optional<ByteVector>> LoadEpochKey(const std::string& call_id, uint32_t media_epoch) const;
  /** Stub wrap: returns base64(key) until pairwise AEAD wiring. */
  Roe<std::string> StubWrapKeyB64(const ByteVector& key_bytes) const;
  Roe<ByteVector> GenerateEpochKey() const;

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> RequireDek() const;

  std::string profile_db_path_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
};

} // namespace pbr
