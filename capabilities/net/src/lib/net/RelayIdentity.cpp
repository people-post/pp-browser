#include "lib/net/RelayIdentity.h"
#include "foundation/tls/RelayClientHeader.h"

#include "common/ValueJson.h"
#include "foundation/error/AppError.h"
#include "lib/net/HttpClient.h"

namespace pbr {

namespace {

/** A 2-of-3 certificate is about 22 KB in base64; the largest one read (kMaxRelayCertificateBytes) fits. */
constexpr size_t kMaxIdentityBodyBytes = 384 * 1024;

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
  if (root->getIf<int64_t>("schema_version").value_or(0) < kRelayIdentitySchemaVersion) {
    return Unavailable(kRelayIdentityInvalid, "schema_version below 2: a relay from before the relay certificate");
  }
  const auto relay_id = RelayId::Parse(root->getString("relay_id").value_or(""));
  if (!relay_id) {
    return Unavailable(kRelayIdentityInvalid, "relay_id is not a relay id");
  }
  std::string certificate_b64 = root->getString("certificate_b64").value_or("");
  if (certificate_b64.empty()) {
    return Unavailable(kRelayIdentityInvalid, "no certificate_b64");
  }
  return RelayIdentity{.relay_id = *relay_id, .certificate_b64 = std::move(certificate_b64)};
}

Roe<RelayIdentity> FetchRelayIdentity(const std::string& base_url, const bool public_only) {
  if (base_url.empty()) {
    return Unavailable(kRelayIdentityUnavailable, "no relay");
  }
  const auto response = HttpClient::Get(base_url + "/v1/relay-identity", WithRelayClientHeader(), kMaxIdentityBodyBytes,
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
