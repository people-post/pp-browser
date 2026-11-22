#include "lib/delivery/TypedSigning.h"

#include "foundation/crypto/CryptoUtil.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> SignTypedB64(const RawSignFn& sign, const std::vector<uint8_t>& bytes) {
  if (!sign) {
    return Error("No signer");
  }
  auto raw_b64 = sign(bytes);
  if (!raw_b64) {
    return raw_b64.error();
  }
  auto raw = Base64Decode(*raw_b64);
  if (!raw) {
    return raw.error();
  }
  auto typed = TypedSignature(*raw);
  if (!typed) {
    return typed.error();
  }
  return Base64Encode(*typed);
}

} // namespace pbr
