#include "base/p2p/CallMediaAdpKey.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

#include <cstring>

namespace pbr {

Roe<adp::PeerKey> DeriveCallMediaAdpAssocKey(const ByteVector& media_key, const std::string& call_id,
                                             const uint32_t media_epoch) {
  if (media_key.empty()) {
    return Error("adp: empty media key");
  }
  EnsureSodiumInit();
  const std::string info =
      std::string("pp-adp-call-media-v1|call_id:") + call_id + "|epoch:" + std::to_string(media_epoch);
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kHkdfSalt),
                                     std::strlen(kHkdfSalt), media_key.data(), media_key.size()) != 0) {
    return Error("adp: HKDF extract failed");
  }
  adp::PeerKey out{};
  if (crypto_kdf_hkdf_sha256_expand(out.bytes.data(), out.bytes.size(), info.c_str(), info.size(), prk) !=
      0) {
    return Error("adp: HKDF expand failed");
  }
  return out;
}

adp::AssocId MintCallMediaAdpAssocId() {
  adp::AssocId id{};
  EnsureSodiumInit();
  randombytes_buf(id.bytes.data(), id.bytes.size());
  return id;
}

std::string AssocIdToHex(const adp::AssocId& id) {
  return BytesToHex(ByteVector(id.bytes.begin(), id.bytes.end()));
}

Roe<adp::AssocId> AssocIdFromHex(const std::string& hex) {
  auto bytes = HexToBytes(hex);
  if (!bytes) {
    return bytes.error();
  }
  if (bytes->size() != adp::kAssocIdBytes) {
    return Error("adp: assoc hex wrong length");
  }
  adp::AssocId id{};
  std::memcpy(id.bytes.data(), bytes->data(), adp::kAssocIdBytes);
  return id;
}

} // namespace pbr
