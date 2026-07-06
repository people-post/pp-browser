#include "base/net/RegistrationSignPayload.h"

#include "base/crypto/CryptoUtil.h"
#include "base/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kRegisterDomain[] = "pp-browser:relay-register-v1";
constexpr const char kProfileDomain[] = "pp-browser:relay-profile-v1";
constexpr uint8_t kSignVersion = 1;

} // namespace

std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& challenge, const std::string& public_key_b64,
                                                const std::string& signature_alg, const int64_t timestamp) {
  const auto public_key = Base64Decode(public_key_b64);
  if (!public_key || public_key.value().size() != 32) {
    return {};
  }

  std::ostringstream oss;
  RelaySignAppendDomain(oss, kRegisterDomain);
  RelaySignAppendU8(oss, kSignVersion);
  RelaySignAppendWireLenUtf8(oss, challenge);
  oss.write(reinterpret_cast<const char*>(public_key.value().data()),
            static_cast<std::streamsize>(public_key.value().size()));
  RelaySignAppendU8(oss, SignatureAlgToWire(signature_alg));
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& relay_user_id, const std::string& nickname,
                                                 const int64_t timestamp) {
  std::ostringstream oss;
  RelaySignAppendDomain(oss, kProfileDomain);
  RelaySignAppendU8(oss, kSignVersion);
  RelaySignAppendWireLenUtf8(oss, relay_user_id);
  RelaySignAppendWireLenUtf8(oss, nickname);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

} // namespace pbr
