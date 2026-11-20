#include "lib/net/RelayIdentity.h"
#include "foundation/tls/RelayClientHeader.h"

#include "common/ValueJson.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/error/AppError.h"
#include "lib/net/HttpClient.h"

#include "crypto/PeerId.h"

namespace pbr {

namespace {

constexpr int64_t kSchemaVersion = 1;
constexpr char kSignatureAlg[] = "ml-dsa-65";

Error Unavailable(const char* code, const std::string& detail) {
  return AppError::Network(Err::Network::RelayIdentityUnavailable, std::string(code) + ": " + detail);
}

} // namespace

Roe<RelayIdentity> ParseRelayIdentity(const std::string& body) {
  const auto root = TryParseObject(body);
  if (!root) {
    return Unavailable(kRelayIdentityInvalid, "not a JSON object");
  }
  // A newer schema_version is read for the fields this reader knows (T15): the relay only adds fields to it.
  if (root->getIf<int64_t>("schema_version").value_or(0) < kSchemaVersion) {
    return Unavailable(kRelayIdentityInvalid, "unsupported schema_version");
  }
  if (root->getString("signature_alg").value_or("") != kSignatureAlg) {
    return Unavailable(kRelayIdentityInvalid, "unsupported signature_alg");
  }
  RelayIdentity identity{.peer_id = root->getString("peer_id").value_or(""),
                         .public_key_b64 = root->getString("public_key_b64").value_or("")};
  const auto key = Base64Decode(identity.public_key_b64);
  if (!key || key.value().size() != kMlDsa65PublicKeyBytes || Base64Encode(key.value()) != identity.public_key_b64) {
    return Unavailable(kRelayIdentityInvalid, "public_key_b64 is not a canonical ML-DSA-65 key");
  }
  const auto derived = pp::PeerIdFromMlDsaPublicKey(key.value());
  if (!derived || *derived != identity.peer_id) {
    return Unavailable(kRelayIdentityInvalid, "peer_id is not the key's");
  }
  return identity;
}

Roe<RelayIdentity> FetchRelayIdentity(const std::string& base_url, const bool public_only) {
  if (base_url.empty()) {
    return Unavailable(kRelayIdentityUnavailable, "no relay");
  }
  const auto response = HttpClient::Get(base_url + "/v1/relay-identity", WithRelayClientHeader(), 64 * 1024,
                                        HttpTimeout{.total_s = 10, .connect_s = 5}, public_only);
  if (!response) {
    return Unavailable(kRelayIdentityUnavailable, response.error().message);
  }
  if (response.value().status_code != 200) {
    return Unavailable(kRelayIdentityUnavailable, "HTTP " + std::to_string(response.value().status_code));
  }
  return ParseRelayIdentity(response.value().body);
}

} // namespace pbr
