#include "app/node/NodeConfigLoad.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path WriteConfig(const std::string& json) {
  const auto path =
      std::filesystem::temp_directory_path() /
      ("pp-node-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
  std::ofstream(path) << json;
  return path;
}

pbr::Roe<pbr::NodeConfig> Load(const std::filesystem::path& path) {
  std::string arg0 = "pp-node";
  std::string flag = "--config";
  std::string file = path.string();
  std::vector<char*> argv = {arg0.data(), flag.data(), file.data(), nullptr};
  return pbr::LoadNodeConfig(3, argv.data());
}

} // namespace

// capability-repos CR028: the node reads these keys of the file, and nothing else of it. A loader of the node's own
// (CR028 step 2) must keep this test passing unchanged.
TEST(NodeConfigTest, TheNodeReadsItsKeysOfTheFile) {
  const auto path = WriteConfig(R"({
    "config_version": 1,
    "theme": "themes/dark.rcss",
    "llm": { "model": "some-model" },
    "data_dir": "/var/lib/pp-node",
    "relay": { "content_sha256": true },
    "registration": { "base_url": "https://backend.example.test/api/relay", "relay_id": "relay:abc" },
    "mesh": { "amp_udp_port": 443, "mesh_publish": true, "advertise_multiaddrs": ["/ip4/203.0.113.5/udp/443/adp/2"],
              "capabilities": { "circuit_relay": true, "media_relay": false, "dht": true } },
    "node": { "relay": { "enabled": true, "retention_days": 3 } }
  })");
  auto loaded = Load(path);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  const pbr::NodeConfig& config = *loaded;
  EXPECT_EQ(config.data_dir, "/var/lib/pp-node");
  EXPECT_TRUE(config.relay_content_sha256);
  // On a built-in backend the running backend's registration replaces the file's (the app's loader: backend override);
  // a deployment names its own with PP_NODE_REGISTRATION_BASE_URL / _RELAY_ID, applied after loading.
  EXPECT_NE(config.registration.base_url, "https://backend.example.test/api/relay");
  EXPECT_NE(config.registration.relay_id, "relay:abc");
  EXPECT_EQ(config.mesh.amp_udp_port, 443);
  EXPECT_TRUE(config.mesh.mesh_publish);
  ASSERT_EQ(config.mesh.advertise_multiaddrs.size(), 1u);
  EXPECT_TRUE(config.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(config.mesh.capabilities.media_relay);
  EXPECT_TRUE(config.mesh.capabilities.dht);
  const pbr::Object* relay = config.node.getObject("relay");
  ASSERT_NE(relay, nullptr);
  EXPECT_EQ(relay->getIf<int64_t>("retention_days"), 3);
}

TEST(NodeConfigTest, ANewerFileIsRefused) {
  const auto path = WriteConfig(R"({ "config_version": 99 })");
  auto loaded = Load(path);
  std::filesystem::remove(path);
  ASSERT_FALSE(static_cast<bool>(loaded));
  EXPECT_NE(loaded.error().message.find("Unsupported config version 99"), std::string::npos);
}

TEST(NodeConfigTest, NoFileNamedIsTheDefaults) {
  std::string arg0 = "pp-node";
  std::vector<char*> argv = {arg0.data(), nullptr};
  auto loaded = pbr::LoadNodeConfig(1, argv.data());
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  EXPECT_TRUE(loaded->data_dir.empty());
  EXPECT_TRUE(loaded->node.empty());
  EXPECT_TRUE(pbr::NodeConfigPath(1, argv.data()).empty());
}
