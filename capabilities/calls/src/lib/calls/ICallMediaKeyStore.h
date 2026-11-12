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
 * fresh key is calls', on top. The key travels to a peer inside the control-lane copy sealed for its device (M024).
 * The app's storage keeps them (encrypted at rest); tests use an in-memory one.
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
};

} // namespace pbr
