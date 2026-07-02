#include "base/crypto/PskFingerprint.h"

#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"

#include <sodium.h>

#include <sstream>

namespace pbr {

Roe<ByteVector> PskFingerprint::Compute(const ByteVector& master_psk) {
  if (master_psk.size() != kMasterPskSize) {
    return Error("Invalid master PSK size");
  }
  EnsureSodiumInit();
  ByteVector digest(kPskFingerprintSize);
  if (crypto_generichash(digest.data(), digest.size(), master_psk.data(), master_psk.size(), nullptr, 0) != 0) {
    return Error("BLAKE2b fingerprint failed");
  }
  return digest;
}

std::string PskFingerprint::FormatDisplay(const ByteVector& digest) {
  const std::string hex = BytesToHex(digest);
  std::ostringstream out;
  for (size_t i = 0; i < hex.size(); i += 4) {
    if (i > 0) {
      out << ' ';
    }
    out << hex.substr(i, 4);
  }
  return out.str();
}

Roe<ByteVector> PskFingerprint::ParseDisplay(const std::string_view grouped_hex) {
  std::string compact;
  compact.reserve(grouped_hex.size());
  for (const char c : grouped_hex) {
    if (c == ' ' || c == '-') {
      continue;
    }
    compact.push_back(c);
  }
  return HexToBytes(compact);
}

} // namespace pbr
