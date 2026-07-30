#include "base/data/ConfigJson.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

TEST(ConfigJsonTest, ParsesLegacyAndModernFields) {
  const nlohmann::json legacy = nlohmann::json::parse(R"({
    "mcp": { "url": "https://legacy.example/mcp" },
    "mcp_servers": [
      { "id": "custom-a", "url": "https://custom.example/mcp" }
    ]
  })");

  pbr::AppConfig config;
  pbr::from_json(legacy, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://legacy.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].id, "custom-a");

  const nlohmann::json modern = nlohmann::json::parse(R"({
    "promoted_mcp": { "url": "https://promoted.example/mcp" },
    "mcp_servers": [
      { "id": "b", "command": "mock", "enabled": true }
    ],
    "relay": { "base_url": "https://relay.example" },
    "directory": { "base_url": "" },
    "registration": { "base_url": "" }
  })");

  pbr::from_json(modern, config);
  EXPECT_EQ(config.promoted_mcp.url, "https://promoted.example/mcp");
  ASSERT_EQ(config.mcp_servers.size(), 1u);
  EXPECT_EQ(config.mcp_servers[0].command, "mock");
  EXPECT_EQ(config.relay.base_url, "https://relay.example");
}

TEST(ConfigJsonTest, EmitsModernConfigKeys) {
  pbr::AppConfig config;
  config.promoted_mcp.url = "https://promoted.example/mcp";

  nlohmann::json out;
  pbr::to_json(out, config);
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

TEST(ConfigJsonTest, RoundTripsLibp2pRoleFields) {
  pbr::AppConfig config;
  config.libp2p.node_enabled = false;
  config.libp2p.listen_multiaddr = "/ip4/0.0.0.0/tcp/18520";
  config.libp2p.bootstrap_peers = {
      "/ip4/3.208.41.58/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  config.libp2p.prefer_contacts_for_routing = false;
  config.libp2p.capabilities.circuit_relay = true;
  config.libp2p.capabilities.media_relay = false;
  config.libp2p.pricing.media_relay.mode = "volunteer";
  config.libp2p.media_relay_budget.default_per_user_up_bps = 12345;

  nlohmann::json out;
  pbr::to_json(out, config);
  ASSERT_TRUE(out.contains("libp2p"));
  EXPECT_EQ(out["libp2p"]["node_enabled"], false);
  EXPECT_EQ(out["libp2p"]["listen_multiaddr"], "/ip4/0.0.0.0/tcp/18520");
  ASSERT_EQ(out["libp2p"]["bootstrap_peers"].size(), 1u);
  EXPECT_EQ(out["libp2p"]["prefer_contacts_for_routing"], false);
  EXPECT_EQ(out["libp2p"]["capabilities"]["circuit_relay"], true);
  EXPECT_EQ(out["libp2p"]["capabilities"]["media_relay"], false);

  pbr::AppConfig parsed;
  pbr::from_json(out, parsed);
  EXPECT_FALSE(parsed.libp2p.node_enabled);
  EXPECT_EQ(parsed.libp2p.listen_multiaddr, "/ip4/0.0.0.0/tcp/18520");
  ASSERT_EQ(parsed.libp2p.bootstrap_peers.size(), 1u);
  EXPECT_EQ(parsed.libp2p.bootstrap_peers[0], config.libp2p.bootstrap_peers[0]);
  EXPECT_FALSE(parsed.libp2p.prefer_contacts_for_routing);
  EXPECT_TRUE(parsed.libp2p.capabilities.circuit_relay);
  EXPECT_FALSE(parsed.libp2p.capabilities.media_relay);
  EXPECT_EQ(parsed.libp2p.media_relay_budget.default_per_user_up_bps, 12345);
}
