#include "app/node/NodeEnvOverlay.h"

#include <gtest/gtest.h>

#include <cstdlib>

namespace {

#if defined(_WIN32)
void SetEnv(const char* name, const char* value) {
  _putenv_s(name, value);
}
void UnsetEnv(const char* name) {
  _putenv_s(name, "");
}
#else
void SetEnv(const char* name, const char* value) {
  setenv(name, value, 1);
}
void UnsetEnv(const char* name) {
  unsetenv(name);
}
#endif

struct EnvGuard {
  std::string name;
  explicit EnvGuard(const char* n) : name(n) { UnsetEnv(name.c_str()); }
  ~EnvGuard() { UnsetEnv(name.c_str()); }
};

} // namespace

TEST(NodeEnvOverlayTest, ParsesBoolEnv) {
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("1"), true);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("TRUE"), true);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv(" yes "), true);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("on"), true);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("0"), false);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("No"), false);
  EXPECT_EQ(pbr::ParsePpNodeBoolEnv("off"), false);
  EXPECT_FALSE(pbr::ParsePpNodeBoolEnv("").has_value());
  EXPECT_FALSE(pbr::ParsePpNodeBoolEnv("maybe").has_value());
}

TEST(NodeEnvOverlayTest, ParsesBootstrapPeersCsv) {
  const auto peers =
      pbr::ParsePpNodeBootstrapPeersCsv(" /ip4/1.2.3.4/udp/443/adp/2/p2p/abc ,,/ip4/5.6.7.8/udp/443/adp/2/p2p/def ");
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0], "/ip4/1.2.3.4/udp/443/adp/2/p2p/abc");
  EXPECT_EQ(peers[1], "/ip4/5.6.7.8/udp/443/adp/2/p2p/def");
  EXPECT_TRUE(pbr::ParsePpNodeBootstrapPeersCsv("").empty());
}

TEST(NodeEnvOverlayTest, AppliesConfigEnvOverlays) {
  EnvGuard g1("PP_NODE_DATA_DIR");
  EnvGuard g3("PP_NODE_BOOTSTRAP_PEERS");
  EnvGuard g4("PP_NODE_CAP_CIRCUIT_RELAY");
  EnvGuard g5("PP_NODE_CAP_MEDIA_RELAY");
  EnvGuard g5b("PP_NODE_CAP_DHT");
  EnvGuard g5c("PP_NODE_CAP_LEDGER_GATEWAY");
  EnvGuard g6("PP_NODE_ADVERTISE_MULTIADDRS");
  EnvGuard g7("PP_NODE_MESH_PUBLISH");
  EnvGuard g7b("PP_NODE_MESH_HEARTBEAT");
  EnvGuard g8("PP_NODE_AMP_UDP_PORT");
  EnvGuard g9("PP_NODE_REGISTRATION_BASE_URL");
  EnvGuard g10("PP_NODE_DHT_K");

  pbr::AppConfig config;
  config.data_dir = "/old";
  config.mesh.bootstrap_peers = {"/ip4/9.9.9.9/udp/443/adp/2/p2p/x"};
  config.mesh.amp_udp_port = 0;
  config.mesh.capabilities.circuit_relay = false;
  config.mesh.capabilities.media_relay = false;
  config.mesh.capabilities.dht = false;
  config.mesh.capabilities.ledger_gateway = false;
  config.mesh.mesh_publish = false;
  config.registration.base_url = "https://backend.example.test/api/relay";

  SetEnv("PP_NODE_DATA_DIR", "/var/lib/pp-node");
  SetEnv("PP_NODE_BOOTSTRAP_PEERS", "/ip4/1.1.1.1/udp/443/adp/2/p2p/a,/ip4/2.2.2.2/udp/443/adp/2/p2p/b");
  SetEnv("PP_NODE_AMP_UDP_PORT", "443");
  SetEnv("PP_NODE_CAP_CIRCUIT_RELAY", "true");
  SetEnv("PP_NODE_CAP_MEDIA_RELAY", "0");
  SetEnv("PP_NODE_CAP_DHT", "yes");
  SetEnv("PP_NODE_CAP_LEDGER_GATEWAY", "1");
  SetEnv("PP_NODE_ADVERTISE_MULTIADDRS", "/ip4/8.8.8.8/udp/443/adp/2/p2p/seed");
  SetEnv("PP_NODE_REGISTRATION_BASE_URL", "https://backend.example.test/api/relay");
  SetEnv("PP_NODE_DHT_K", "3");

  pbr::ApplyPpNodeConfigEnvOverlays(config);

  EXPECT_EQ(config.data_dir, "/var/lib/pp-node");
  EXPECT_EQ(config.mesh.amp_udp_port, 443);
  ASSERT_EQ(config.mesh.bootstrap_peers.size(), 2u);
  EXPECT_EQ(config.mesh.bootstrap_peers[0], "/ip4/1.1.1.1/udp/443/adp/2/p2p/a");
  EXPECT_TRUE(config.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(config.mesh.capabilities.media_relay);
  EXPECT_TRUE(config.mesh.capabilities.dht);
  EXPECT_TRUE(config.mesh.capabilities.ledger_gateway);
  ASSERT_EQ(config.mesh.advertise_multiaddrs.size(), 1u);
  EXPECT_EQ(config.mesh.advertise_multiaddrs[0], "/ip4/8.8.8.8/udp/443/adp/2/p2p/seed");
  EXPECT_TRUE(config.mesh.mesh_publish);
  EXPECT_FALSE(config.mesh.mesh_heartbeat) << "publishing does not turn the heartbeat on (P011)";
  EXPECT_EQ(config.registration.base_url, "https://backend.example.test/api/relay");
  EXPECT_EQ(config.mesh.dht.k_bucket_size, 3);
}

// The heartbeat tells the directory when this node is up and from where: only an operator's explicit choice
// turns it on (privacy P011).
TEST(NodeEnvOverlayTest, TheHeartbeatIsTheOperatorsChoice) {
  EnvGuard heartbeat("PP_NODE_MESH_HEARTBEAT");
  pbr::AppConfig config;
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_FALSE(config.mesh.mesh_heartbeat);
  SetEnv("PP_NODE_MESH_HEARTBEAT", "1");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_TRUE(config.mesh.mesh_heartbeat);
}

// relay.content_sha256 is off unless config or the environment turns it on; an unparsable value keeps the config's.
TEST(NodeEnvOverlayTest, RelayContentSha256ComesFromTheEnvironment) {
  EnvGuard guard("PP_NODE_RELAY_CONTENT_SHA256");
  pbr::AppConfig config;
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_FALSE(config.relay_content_sha256);
  SetEnv("PP_NODE_RELAY_CONTENT_SHA256", "true");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_TRUE(config.relay_content_sha256);
  SetEnv("PP_NODE_RELAY_CONTENT_SHA256", "maybe");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_TRUE(config.relay_content_sha256);
  SetEnv("PP_NODE_RELAY_CONTENT_SHA256", "0");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_FALSE(config.relay_content_sha256);
}

// mesh.capacity from the environment: a positive count, anything else keeps what the config said.
TEST(NodeEnvOverlayTest, CapacitiesComeFromTheEnvironment) {
  EnvGuard links("PP_NODE_CAPACITY_LINKS");
  EnvGuard reservations("PP_NODE_CAPACITY_RESERVATIONS");
  EnvGuard bridges("PP_NODE_CAPACITY_BRIDGES");
  EnvGuard calls("PP_NODE_CAPACITY_GROUP_CALLS");
  pbr::AppConfig config;
  config.mesh.capacity.bridges = 100;
  SetEnv("PP_NODE_CAPACITY_LINKS", "20000");
  SetEnv("PP_NODE_CAPACITY_RESERVATIONS", "40");
  SetEnv("PP_NODE_CAPACITY_BRIDGES", "-1");
  SetEnv("PP_NODE_CAPACITY_GROUP_CALLS", "many");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_EQ(config.mesh.capacity.links, 20000u);
  EXPECT_EQ(config.mesh.capacity.reservations, 40u);
  EXPECT_EQ(config.mesh.capacity.bridges, 100u) << "a bad value keeps the config's";
  EXPECT_EQ(config.mesh.capacity.group_calls, 0u);
}

constexpr const char kQaRelayId[] = "relay:AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE";
constexpr const char kProdRelayId[] = "relay:AgICAgICAgICAgICAgICAgICAgICAgICAgICAgICAgI";

// Multi-relay MR008 item 5, SIGNED_FORMATS § 3.3: the relay the node registers at is expected to prove
// `registration.relay_id` (PP_NODE_REGISTRATION_RELAY_ID), defaulting to the compiled prod relay's id with the
// compiled prod URL only.
TEST(NodeEnvOverlayTest, TheRegistrationRelaysIdComesFromItsAddress) {
  EnvGuard url("PP_NODE_REGISTRATION_BASE_URL");
  EnvGuard relay_id("PP_NODE_REGISTRATION_RELAY_ID");
  const std::string prod = "https://backend.example.test/api/relay";
  pbr::AppConfig config;
  config.registration.base_url = prod;
  config.registration.relay_id = kProdRelayId;
  SetEnv("PP_NODE_REGISTRATION_BASE_URL", "https://qa.example.test/api/relay");
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_TRUE(config.registration.relay_id.empty()) << "another relay: the file's relay id was the old one's";
  SetEnv("PP_NODE_REGISTRATION_RELAY_ID", kQaRelayId);
  pbr::ApplyPpNodeConfigEnvOverlays(config);
  EXPECT_EQ(config.registration.relay_id, kQaRelayId);

  pbr::ServiceEndpointConfig compiled{.base_url = prod + "/"};
  pbr::DefaultNodeRegistrationRelayId(compiled, prod, kProdRelayId);
  EXPECT_EQ(compiled.relay_id, kProdRelayId) << "the compiled URL: the compiled relay id";
  pbr::ServiceEndpointConfig other{.base_url = "https://other.example.test/api/relay"};
  pbr::DefaultNodeRegistrationRelayId(other, prod, kProdRelayId);
  EXPECT_TRUE(other.relay_id.empty()) << "another URL: none (first use, with a warning)";
  pbr::ServiceEndpointConfig named{.base_url = prod, .relay_id = kQaRelayId};
  pbr::DefaultNodeRegistrationRelayId(named, prod, kProdRelayId);
  EXPECT_EQ(named.relay_id, kQaRelayId) << "a named one stays";
}

// MR008 item 5: outside a developer build the node refuses to register at a relay its address names no relay id for,
// and says which setting is missing; the build's placeholder or a value not of the form is refused in every build; the
// compiled prod URL, however spelled, takes the compiled relay id.
TEST(NodeEnvOverlayTest, ARegistrationRelayWithoutItsRelayIdIsRefusedOutsideDeveloperBuilds) {
  const pbr::ServiceEndpointConfig unnamed{.base_url = "https://relay.example.test/api/relay"};
  const std::string problem = pbr::NodeRegistrationIdProblem(unnamed, false);
  EXPECT_NE(problem.find("registration.relay_id"), std::string::npos) << problem;
  EXPECT_NE(problem.find("PP_NODE_REGISTRATION_RELAY_ID"), std::string::npos) << problem;
  EXPECT_EQ(pbr::NodeRegistrationIdProblem(unnamed, true), "") << "a developer build: first use, warned";
  EXPECT_EQ(pbr::NodeRegistrationIdProblem({.base_url = unnamed.base_url, .relay_id = kQaRelayId}, false), "");
  EXPECT_EQ(pbr::NodeRegistrationIdProblem({}, false), "") << "no registration URL: nothing to refuse";
  const pbr::ServiceEndpointConfig pending{.base_url = unnamed.base_url, .relay_id = "pending"};
  EXPECT_NE(pbr::NodeRegistrationIdProblem(pending, true).find("placeholder"), std::string::npos)
      << "the placeholder fails closed, a developer build too";
  const pbr::ServiceEndpointConfig peer_id{.base_url = unnamed.base_url,
                                           .relay_id = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP"};
  EXPECT_NE(pbr::NodeRegistrationIdProblem(peer_id, true), "") << "a PeerId is no relay id";

  // The user-run Pi's env (`PP_NODE_REGISTRATION_BASE_URL`), spelled as an operator might: the compiled URL.
  const std::string compiled = "https://api.pp.peoplepost.org/api/relay";
  for (const char* spelled :
       {"https://api.pp.peoplepost.org/api/relay", "https://api.pp.peoplepost.org/api/relay/",
        "HTTPS://API.pp.peoplepost.org:443/api/relay", " https://api.pp.peoplepost.org/api/relay "}) {
    pbr::ServiceEndpointConfig registration{.base_url = spelled};
    pbr::DefaultNodeRegistrationRelayId(registration, compiled, kProdRelayId);
    EXPECT_EQ(registration.relay_id, kProdRelayId) << spelled;
  }
  pbr::ServiceEndpointConfig qa{.base_url = "https://api-qa.pp.peoplepost.org/api/relay"};
  pbr::DefaultNodeRegistrationRelayId(qa, compiled, kProdRelayId);
  EXPECT_TRUE(qa.relay_id.empty()) << "QA is another relay: PP_NODE_REGISTRATION_RELAY_ID names it";
}
