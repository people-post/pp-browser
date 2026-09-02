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
  const auto peers = pbr::ParsePpNodeBootstrapPeersCsv(
      " /ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/abc ,,/ip4/5.6.7.8/udp/443/adp/1.0.0/p2p/def ");
  ASSERT_EQ(peers.size(), 2u);
  EXPECT_EQ(peers[0], "/ip4/1.2.3.4/udp/443/adp/1.0.0/p2p/abc");
  EXPECT_EQ(peers[1], "/ip4/5.6.7.8/udp/443/adp/1.0.0/p2p/def");
  EXPECT_TRUE(pbr::ParsePpNodeBootstrapPeersCsv("").empty());
}

TEST(NodeEnvOverlayTest, AppliesConfigEnvOverlays) {
  EnvGuard g1("PP_NODE_DATA_DIR");
  EnvGuard g3("PP_NODE_BOOTSTRAP_PEERS");
  EnvGuard g4("PP_NODE_CAP_CIRCUIT_RELAY");
  EnvGuard g5("PP_NODE_CAP_MEDIA_RELAY");
  EnvGuard g5b("PP_NODE_CAP_DHT");
  EnvGuard g6("PP_NODE_ADVERTISE_MULTIADDRS");
  EnvGuard g7("PP_NODE_MESH_PUBLISH");
  EnvGuard g8("PP_NODE_AMP_UDP_PORT");
  EnvGuard g9("PP_NODE_REGISTRATION_BASE_URL");

  pbr::AppConfig config;
  config.data_dir = "/old";
  config.mesh.bootstrap_peers = {"/ip4/9.9.9.9/udp/443/adp/1.0.0/p2p/x"};
  config.mesh.amp_udp_port = 0;
  config.mesh.capabilities.circuit_relay = false;
  config.mesh.capabilities.media_relay = false;
  config.mesh.capabilities.dht = false;
  config.mesh.mesh_publish = false;
  config.registration.base_url = "https://www.brief.global/api/relay";

  SetEnv("PP_NODE_DATA_DIR", "/var/lib/pp-node");
  SetEnv("PP_NODE_BOOTSTRAP_PEERS",
         "/ip4/1.1.1.1/udp/443/adp/1.0.0/p2p/a,/ip4/2.2.2.2/udp/443/adp/1.0.0/p2p/b");
  SetEnv("PP_NODE_AMP_UDP_PORT", "443");
  SetEnv("PP_NODE_CAP_CIRCUIT_RELAY", "true");
  SetEnv("PP_NODE_CAP_MEDIA_RELAY", "0");
  SetEnv("PP_NODE_CAP_DHT", "yes");
  SetEnv("PP_NODE_ADVERTISE_MULTIADDRS", "/ip4/8.8.8.8/udp/443/adp/1.0.0/p2p/seed");
  SetEnv("PP_NODE_REGISTRATION_BASE_URL", "https://www-en.qa.peoplepost.org/api/relay");

  pbr::ApplyPpNodeConfigEnvOverlays(config);

  EXPECT_EQ(config.data_dir, "/var/lib/pp-node");
  EXPECT_EQ(config.mesh.amp_udp_port, 443);
  ASSERT_EQ(config.mesh.bootstrap_peers.size(), 2u);
  EXPECT_EQ(config.mesh.bootstrap_peers[0], "/ip4/1.1.1.1/udp/443/adp/1.0.0/p2p/a");
  EXPECT_TRUE(config.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(config.mesh.capabilities.media_relay);
  EXPECT_TRUE(config.mesh.capabilities.dht);
  ASSERT_EQ(config.mesh.advertise_multiaddrs.size(), 1u);
  EXPECT_EQ(config.mesh.advertise_multiaddrs[0], "/ip4/8.8.8.8/udp/443/adp/1.0.0/p2p/seed");
  EXPECT_TRUE(config.mesh.mesh_publish);
  EXPECT_EQ(config.registration.base_url, "https://www-en.qa.peoplepost.org/api/relay");
}
