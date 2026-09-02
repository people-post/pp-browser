#include "foundation/crypto/CryptoUtil.h"

#include "crypto/SodiumUtil.h"
#include "common/PbrCompat.h"

namespace pbr {

void EnsureSodiumInit() {
  ::pp::EnsureSodiumInit();
}

Roe<ByteVector> HexToBytes(const std::string_view hex) {
  return ::pp::HexToBytes(hex);
}

std::string BytesToHex(const ByteVector& bytes) {
  return ::pp::BytesToHex(bytes);
}

Roe<ByteVector> Base64Decode(const std::string& encoded) {
  return ::pp::Base64Decode(encoded);
}

std::string Base64Encode(const ByteVector& bytes) {
  return ::pp::Base64Encode(bytes);
}

} // namespace pbr
