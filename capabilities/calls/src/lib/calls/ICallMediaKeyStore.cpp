#include "lib/calls/ICallMediaKeyStore.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>

namespace pbr {

// A call media key's logic (CR024): calls', over any store.

Roe<ByteVector> ICallMediaKeyStore::GenerateEpochKey() const {
  EnsureSodiumInit();
  ByteVector key(32);
  randombytes_buf(key.data(), key.size());
  return key;
}

} // namespace pbr
