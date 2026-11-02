#include "lib/calls/ICallMediaKeyStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/calls/MediaKeyWrap.h"

#include <sodium.h>

namespace pbr {

// A call media key's logic (CR024): calls', over any store.

Roe<ByteVector> ICallMediaKeyStore::GenerateEpochKey() const {
  EnsureSodiumInit();
  ByteVector key(32);
  randombytes_buf(key.data(), key.size());
  return key;
}

std::string ICallMediaKeyStore::BuildWrapAad(const std::string& call_id, const uint32_t media_epoch,
                                             const std::string& media_key_id) {
  return MediaKeyWrapAad(kCallMediaKeyWrapLabel, call_id, media_epoch, media_key_id);
}

Roe<std::string> ICallMediaKeyStore::WrapKeyB64(const ByteVector& session_key, const ByteVector& key_bytes,
                                                const std::string& call_id, const uint32_t media_epoch,
                                                const std::string& media_key_id) {
  return WrapMediaKeyB64(kCallMediaKeyWrapLabel, session_key, key_bytes, call_id, media_epoch, media_key_id);
}

Roe<ByteVector> ICallMediaKeyStore::UnwrapKeyB64(const ByteVector& session_key, const std::string& wrapped_key_b64,
                                                 const std::string& call_id, const uint32_t media_epoch,
                                                 const std::string& media_key_id) {
  return UnwrapMediaKeyB64(kCallMediaKeyWrapLabel, session_key, wrapped_key_b64, call_id, media_epoch, media_key_id);
}

} // namespace pbr
