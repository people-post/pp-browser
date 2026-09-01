#include "base/data/ConfigJson.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

TEST(ConfigJsonTest, ParsesLegacyAndModernFields) {
  auto legacy = pbr::TryParseObject(R"({
    "mcp": { "url": "https://legacy.example/mcp" },
    "mcp_servers": [
      { "id": "custom-a", "url": "https://custom.example/mcp" }
    ]
  })");
  ASSERT_TRUE(legacy.has_value());

  pbr::AppConfig config;
  pbr::AppConfigFromObject(*legacy, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://legacy.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].id, "custom-a");

  auto modern = pbr::TryParseObject(R"({
    "promoted_mcp": { "url": "https://promoted.example/mcp" },
    "mcp_servers": [
      { "id": "b", "command": "mock", "enabled": true }
    ],
    "relay": { "base_url": "https://relay.example" },
    "directory": { "base_url": "" },
    "registration": { "base_url": "" }
  })");
  ASSERT_TRUE(modern.has_value());

  pbr::AppConfigFromObject(*modern, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://promoted.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].command, "mock");
  EXPECT_EQ(config.relay.base_url, "https://relay.example");
}

TEST(ConfigJsonTest, EmitsModernConfigKeys) {
  pbr::AppConfig config;
  config.promoted_mcp.url = "https://promoted.example/mcp";

  const pbr::Object out = pbr::AppConfigToObject(config);
  EXPECT_TRUE(out.contains("promoted_mcp"));
  EXPECT_FALSE(out.contains("mcp"));
}

TEST(ConfigJsonTest, ResolvesPromotedMcpFallbacks) {
  pbr::AppConfig defaults;
  defaults.promoted_mcp.url = "https://www.brief.global/mcp";

  EXPECT_EQ(pbr::ResolvePromotedMcp(defaults, defaults).url, "https://www.brief.global/mcp");

  pbr::AppConfig empty_promoted;
  EXPECT_EQ(pbr::ResolvePromotedMcp(empty_promoted, defaults).url, "https://www.brief.global/mcp");
}

TEST(ConfigJsonTest, RoundTripsMeshRoleFields) {
  pbr::AppConfig config;
  config.mesh.node_enabled = false;
  config.mesh.bootstrap_peers = {
      "/ip4/3.208.41.58/udp/443/adp/1.0.0/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  config.mesh.prefer_contacts_for_routing = false;
  config.mesh.mesh_enabled = false;
  config.mesh.amp_udp_port = 18518;
  config.mesh.capabilities.circuit_relay = true;
  config.mesh.capabilities.media_relay = false;
  config.mesh.pricing.media_relay.mode = "volunteer";
  config.mesh.media_relay_budget.default_per_user_up_bps = 12345;

  const pbr::Object out = pbr::AppConfigToObject(config);
  ASSERT_TRUE(out.contains("mesh"));
  const pbr::Object* mesh = out.getObject("mesh");
  ASSERT_NE(mesh, nullptr);
  EXPECT_EQ(mesh->getIf<bool>("node_enabled"), false);
  EXPECT_FALSE(mesh->contains("listen_multiaddr"));
  EXPECT_FALSE(mesh->contains("enable_amp_stack"));
  EXPECT_FALSE(mesh->contains("max_connections"));
  ASSERT_NE(mesh->getArray("bootstrap_peers"), nullptr);
  EXPECT_EQ(mesh->getArray("bootstrap_peers")->elements.size(), 1u);
  EXPECT_EQ(mesh->getIf<bool>("prefer_contacts_for_routing"), false);
  EXPECT_EQ(mesh->getIf<bool>("mesh_enabled"), false);
  EXPECT_EQ(mesh->getNonNegInt("amp_udp_port"), 18518);
  const pbr::Object* caps = mesh->getObject("capabilities");
  ASSERT_NE(caps, nullptr);
  EXPECT_EQ(caps->getIf<bool>("circuit_relay"), true);
  EXPECT_EQ(caps->getIf<bool>("media_relay"), false);

  pbr::AppConfig parsed;
  pbr::AppConfigFromObject(out, parsed);
  EXPECT_FALSE(parsed.mesh.node_enabled);
  ASSERT_EQ(parsed.mesh.bootstrap_peers.size(), 1u);
  EXPECT_EQ(parsed.mesh.bootstrap_peers[0], config.mesh.bootstrap_peers[0]);
  EXPECT_FALSE(parsed.mesh.prefer_contacts_for_routing);
  EXPECT_FALSE(parsed.mesh.mesh_enabled);
  EXPECT_EQ(parsed.mesh.amp_udp_port, 18518);
  EXPECT_TRUE(parsed.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(parsed.mesh.capabilities.media_relay);
  EXPECT_EQ(parsed.mesh.media_relay_budget.default_per_user_up_bps, 12345);
}
