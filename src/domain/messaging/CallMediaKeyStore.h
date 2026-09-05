#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"

#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * Vault-backed call media key slots (V011).
 * Peer wrap uses pairwise session key + MessageCipher (V015).
 */
class CallMediaKeyStore : public Module, public IDekConsumer {
public:
  explicit CallMediaKeyStore(std::string profile_db_path, std::string profile_id = {});

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;

  /** Persist plaintext key bytes for (call_id, epoch); returns opaque media_key_id. */
  Roe<std::string> PutEpochKey(const std::string& call_id, uint32_t media_epoch, const ByteVector& key_bytes);
  Roe<std::optional<ByteVector>> LoadEpochKey(const std::string& call_id, uint32_t media_epoch) const;
  Roe<ByteVector> GenerateEpochKey() const;

  static std::string BuildWrapAad(const std::string& call_id, uint32_t media_epoch, const std::string& media_key_id);

  /** Wrap epoch key under pairwise session_key → base64 blob (V015). */
  static Roe<std::string> WrapKeyB64(const ByteVector& session_key, const ByteVector& key_bytes,
                                     const std::string& call_id, uint32_t media_epoch,
                                     const std::string& media_key_id);
  static Roe<ByteVector> UnwrapKeyB64(const ByteVector& session_key, const std::string& wrapped_key_b64,
                                      const std::string& call_id, uint32_t media_epoch,
                                      const std::string& media_key_id);

private:
  Roe<sqlite3*> OpenDb() const;
  Roe<void> RequireDek() const;

  std::string profile_db_path_;
  std::string profile_id_;
  mutable ByteVector dek_;
  mutable std::mutex mutex_;
};

} // namespace pbr
