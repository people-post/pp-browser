#include "lib/net/RegistrationSignPayload.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/MlDsa.h"
#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kRegisterDomain[] = "pp-browser:relay-register-v1";
constexpr const char kProfileDomain[] = "pp-browser:relay-profile-v1";
constexpr const char kRevokeDomain[] = "pp-browser:relay-revoke-v1";
constexpr const char kHeartbeatDomain[] = "pp-browser:relay-heartbeat-v1";
constexpr const char kLlmTokenDomain[] = "pp-browser:relay-llm-token-v1";
// MR007: each version names the relay it is for (the audience after the version byte).
constexpr uint8_t kRegisterSignVersion = 3;
constexpr uint8_t kProfileSignVersion = 2;
constexpr uint8_t kRevokeSignVersion = 2;
constexpr uint8_t kHeartbeatSignVersion = 2;
constexpr uint8_t kLlmTokenSignVersion = 2;

void AppendRaw(std::ostringstream& oss, const std::vector<uint8_t>& bytes) {
  oss.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

std::vector<uint8_t> BuildRegistrationSignBytes(const std::string& audience, const std::string& challenge,
                                                const std::string& public_key_b64,
                                                const std::string& kem_public_key_b64, const std::string& signature_alg,
                                                const int64_t timestamp) {
  const auto public_key = Base64Decode(public_key_b64);
  if (!public_key || public_key.value().size() != kMlDsa65PublicKeyBytes) {
    return {};
  }
  const auto kem_public_key = Base64Decode(kem_public_key_b64);
  if (!kem_public_key || kem_public_key.value().size() != kHybridKemPublicKeyBytes) {
    return {};
  }

  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kRegisterDomain, kRegisterSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, challenge);
  AppendRaw(oss, public_key.value());
  AppendRaw(oss, kem_public_key.value());
  RelaySignAppendU8(oss, SignatureAlgToWire(signature_alg));
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildProfileUpdateSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& nickname, const int64_t timestamp) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kProfileDomain, kProfileSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, nickname);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildAccountRevokeSignBytes(const std::string& audience, const std::string& account_id,
                                                 const std::string& registration_id, const int64_t timestamp) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kRevokeDomain, kRevokeSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, registration_id);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildRelayHeartbeatSignBytes(const std::string& audience, const std::string& account_id,
                                                  const std::string& peer_id, const std::string& state,
                                                  const int64_t timestamp) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kHeartbeatDomain, kHeartbeatSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, peer_id);
  RelaySignAppendWireLenUtf8(oss, state);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

std::vector<uint8_t> BuildLlmTokenRenewSignBytes(const std::string& audience, const std::string& account_id,
                                                 const int64_t timestamp) {
  std::ostringstream oss;
  if (!RelaySignAppendCommandHeader(oss, kLlmTokenDomain, kLlmTokenSignVersion, audience)) {
    return {};
  }
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendI64(oss, timestamp);
  return RelaySignOssToBytes(oss);
}

} // namespace pbr
