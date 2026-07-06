#include "base/crypto/CryptoUtil.h"
#include "base/net/ServiceClientsImpl.h"
#include "base/people/Ed25519Signer.h"

#include <cstdlib>
#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {

using namespace pbr;

const char* EnvOrNull(const char* name) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') {
    return nullptr;
  }
  return value;
}

std::optional<std::vector<uint8_t>> LoadSignKeyFromEnv() {
  const char* hex = EnvOrNull("PP_BROWSER_RELAY_INTEGRATION_SIGN_KEY_HEX");
  if (hex == nullptr) {
    return std::nullopt;
  }
  auto bytes = HexToBytes(hex);
  if (!bytes) {
    return std::nullopt;
  }
  return *bytes;
}

} // namespace

TEST(RelayLiveIntegrationTest, FetchChatHistoryAgainstLiveRelayWhenConfigured) {
  const char* base_url = EnvOrNull("PP_BROWSER_RELAY_INTEGRATION_URL");
  if (base_url == nullptr) {
    GTEST_SKIP() << "Set PP_BROWSER_RELAY_INTEGRATION_URL to run live relay integration (D093)";
  }

  const char* requester = EnvOrNull("PP_BROWSER_RELAY_INTEGRATION_REQUESTER");
  const char* peer = EnvOrNull("PP_BROWSER_RELAY_INTEGRATION_PEER");
  if (requester == nullptr || peer == nullptr) {
    GTEST_SKIP() << "Set PP_BROWSER_RELAY_INTEGRATION_REQUESTER and PP_BROWSER_RELAY_INTEGRATION_PEER";
  }

  auto sign_key = LoadSignKeyFromEnv();
  if (!sign_key) {
    GTEST_SKIP() << "Set PP_BROWSER_RELAY_INTEGRATION_SIGN_KEY_HEX (32-byte Ed25519 seed, hex)";
  }

  HttpRelayClient relay(base_url);
  relay.SetAuthSigner([sign_key](const std::vector<uint8_t>& sign_bytes) -> Roe<std::string> {
    return Ed25519Signer::Sign(std::string(sign_bytes.begin(), sign_bytes.end()), *sign_key);
  });

  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = requester;
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = peer;
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.limit = 5;
  request.order = "desc";

  auto response = relay.FetchChatHistory(request);
  ASSERT_TRUE(static_cast<bool>(response)) << response.error().message;
}

TEST(RelayLiveIntegrationTest, HttpRelayRequiresAuthSigner) {
  HttpRelayClient relay("https://relay.example.test");
  ChatHistoryRequest request;
  request.requester_identity_kind = "relay_user";
  request.requester_identity_value = "relay:local";
  request.peer_identity_kind = "relay_user";
  request.peer_identity_value = "relay:peer";
  request.channel = ThreadChannel::E2e;
  request.session_epoch = 1;
  request.limit = 1;

  auto response = relay.FetchChatHistory(request);
  ASSERT_FALSE(static_cast<bool>(response));
  EXPECT_NE(response.error().message.find("auth signer"), std::string::npos);
}
