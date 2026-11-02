#include "foundation/crypto/AttachmentContentHash.h"

#include "foundation/crypto/CryptoUtil.h"

#include <sodium.h>
#include "common/PbrCompat.h"

namespace pbr {

Roe<ByteVector> AttachmentContentHash(const ByteVector& plaintext) {
  EnsureSodiumInit();
  ByteVector digest(kAttachmentContentHashSize);
  if (crypto_generichash(digest.data(), digest.size(), plaintext.data(), plaintext.size(), nullptr, 0) != 0) {
    return Error("Attachment content hash failed");
  }
  return digest;
}

} // namespace pbr
