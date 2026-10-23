#include "foundation/crypto/CryptoUtil.h"

#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <cstring>
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

std::string Base64UrlEncode(const ByteVector& bytes) {
  EnsureSodiumInit();
  std::string out(sodium_base64_ENCODED_LEN(bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING), '\0');
  sodium_bin2base64(out.data(), out.size(), bytes.data(), bytes.size(), sodium_base64_VARIANT_URLSAFE_NO_PADDING);
  out.resize(std::strlen(out.c_str()));
  return out;
}

Roe<ByteVector> Base64UrlDecode(const std::string& encoded) {
  EnsureSodiumInit();
  ByteVector out(encoded.size());
  size_t length = 0;
  if (sodium_base642bin(out.data(), out.size(), encoded.data(), encoded.size(), nullptr, &length, nullptr,
                        sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
    return Error("Invalid base64url");
  }
  out.resize(length);
  return out;
}

} // namespace pbr
