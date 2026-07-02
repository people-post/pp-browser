#include "base/crypto/SessionKeyDeriver.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

#include <cstring>

namespace pbr {

std::string SessionKeyDeriver::BuildHkdfInfo(const CryptoChannel channel, const uint32_t session_epoch) {
  return std::string("channel:") + CryptoChannelToString(channel) + "|epoch:" + std::to_string(session_epoch);
}

Roe<ByteVector> SessionKeyDeriver::Derive(const ByteVector& master_psk, const CryptoChannel channel,
                                          const uint32_t session_epoch) {
  if (master_psk.size() != kMasterPskSize) {
    return Error("Invalid master PSK size");
  }
  EnsureSodiumInit();

  const std::string info = BuildHkdfInfo(channel, session_epoch);
  unsigned char prk[crypto_kdf_hkdf_sha256_KEYBYTES];
  if (crypto_kdf_hkdf_sha256_extract(prk, reinterpret_cast<const unsigned char*>(kHkdfSalt), std::strlen(kHkdfSalt),
                                     master_psk.data(), master_psk.size()) != 0) {
    return Error("HKDF extract failed");
  }
  ByteVector session_key(kSessionKeySize);
  if (crypto_kdf_hkdf_sha256_expand(session_key.data(), session_key.size(), info.c_str(), info.size(), prk) != 0) {
    return Error("HKDF expand failed");
  }
  return session_key;
}

} // namespace pbr
