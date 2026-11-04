#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Where a device keeps its call media keys, one per call and media epoch (V011, CR024): the port is the slots; a
 * fresh key and its wrap for a peer (V015, MediaKeyWrap) are calls', on top. The app's storage keeps them (encrypted
 * at rest); tests use an in-memory one.
 */
class ICallMediaKeyStore {
public:
  virtual ~ICallMediaKeyStore() = default;

  /** Persist plaintext key bytes for (call_id, epoch); returns the opaque media_key_id. */
  virtual Roe<std::string> PutEpochKey(const std::string& call_id, uint32_t media_epoch,
                                       const ByteVector& key_bytes) = 0;
  virtual Roe<std::optional<ByteVector>> LoadEpochKey(const std::string& call_id, uint32_t media_epoch) const = 0;

  /** A fresh random epoch key. */
  Roe<ByteVector> GenerateEpochKey() const;

  /** The label call media keys are wrapped under (MediaKeyWrap; broadcast uses its own). */
  static constexpr const char* kCallMediaKeyWrapLabel = "call_media_key";
  static std::string BuildWrapAad(const std::string& call_id, uint32_t media_epoch, const std::string& media_key_id);
  /** Wrap an epoch key under a pairwise session key → base64 blob (V015). */
  static Roe<std::string> WrapKeyB64(const ByteVector& session_key, const ByteVector& key_bytes,
                                     const std::string& call_id, uint32_t media_epoch,
                                     const std::string& media_key_id);
  static Roe<ByteVector> UnwrapKeyB64(const ByteVector& session_key, const std::string& wrapped_key_b64,
                                      const std::string& call_id, uint32_t media_epoch,
                                      const std::string& media_key_id);
};

} // namespace pbr
