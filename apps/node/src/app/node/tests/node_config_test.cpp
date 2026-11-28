#include "app/node/NodeConfigLoad.h"
#include "common/ValueJson.h"
#include "foundation/data/ConfigSectionsJson.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

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

std::filesystem::path WriteConfig(const std::string& json) {
  static int n = 0;
  const auto path = std::filesystem::temp_directory_path() /
                    ("pp-node-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(++n) + ".json");
  std::ofstream(path) << json;
  return path;
}

/** argv naming `path` (empty: none). */
struct Args {
  std::vector<std::string> owned;
  std::vector<char*> argv;
  explicit Args(const std::string& path) {
    owned = path.empty() ? std::vector<std::string>{"pp-node"} : std::vector<std::string>{"pp-node", "--config", path};
    for (auto& s : owned) {
      argv.push_back(s.data());
    }
    argv.push_back(nullptr);
  }
  int argc() const { return static_cast<int>(owned.size()); }
  char** data() { return argv.data(); }
};

pbr::Roe<pbr::NodeConfig> Load(const std::filesystem::path& path) {
  Args args(path.string());
  return pbr::LoadNodeConfig(args.argc(), args.data());
}

} // namespace

TEST(NodeConfigTest, TheNodeReadsItsKeysOfTheFile) {
  EnvGuard config("PP_BROWSER_CONFIG");
  pbr::ResetActiveBackendForTest();
  const auto path = WriteConfig(R"({
    "config_version": 1,
    "theme": "themes/dark.rcss",
    "llm": { "model": "some-model" },
    "data_dir": "/var/lib/pp-node",
    "relay": { "content_sha256": true },
    "mesh": { "amp_udp_port": 443, "mesh_publish": true, "advertise_multiaddrs": ["/ip4/203.0.113.5/udp/443/adp/2"],
              "capabilities": { "circuit_relay": true, "media_relay": false, "dht": true } },
    "node": { "relay": { "enabled": true, "retention_days": 3 } }
  })");
  auto loaded = Load(path);
  std::filesystem::remove(path);
  pbr::ResetActiveBackendForTest();
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  const pbr::NodeConfig& c = *loaded;
  EXPECT_EQ(c.data_dir, "/var/lib/pp-node");
  EXPECT_TRUE(c.relay_content_sha256);
  EXPECT_EQ(c.mesh.amp_udp_port, 443);
  EXPECT_TRUE(c.mesh.mesh_publish);
  ASSERT_EQ(c.mesh.advertise_multiaddrs.size(), 1u);
  EXPECT_TRUE(c.mesh.capabilities.circuit_relay);
  EXPECT_FALSE(c.mesh.capabilities.media_relay);
  EXPECT_TRUE(c.mesh.capabilities.dht);
  const pbr::Object* relay = c.node.getObject("relay");
  ASSERT_NE(relay, nullptr);
  EXPECT_EQ(relay->getIf<int64_t>("retention_days"), 3);
}

TEST(NodeConfigTest, NoFileNamedIsTheDefaults) {
  EnvGuard config("PP_BROWSER_CONFIG");
  pbr::ResetActiveBackendForTest();
  Args args("");
  auto loaded = pbr::LoadNodeConfig(args.argc(), args.data());
  ASSERT_TRUE(static_cast<bool>(loaded)) << loaded.error().message;
  EXPECT_TRUE(loaded->data_dir.empty());
  EXPECT_TRUE(loaded->node.empty());
  EXPECT_TRUE(pbr::NodeConfigPath(args.argc(), args.data()).empty());
  SetEnv("PP_BROWSER_CONFIG", "/etc/pp-node/config.json");
  EXPECT_EQ(pbr::NodeConfigPath(args.argc(), args.data()), "/etc/pp-node/config.json");
}

// A file's registration that a built-in backend replaces is said, not silently dropped.
TEST(NodeConfigTest, AReplacedRegistrationIsSaid) {
  pbr::NodeConfig loaded;
  loaded.registration.base_url = "https://api.example.test/api/relay";
  loaded.registration.relay_id = "relay:prod";
  auto file = pbr::TryParseObject(R"({ "registration": { "base_url": "https://mine.example.test/api/relay" } })");
  ASSERT_TRUE(file.has_value());
  const auto note = pbr::IgnoredRegistrationNote(*file, loaded, "prod");
  ASSERT_TRUE(note.has_value());
  EXPECT_NE(note->find("ignored: backend 'prod' uses its own (https://api.example.test/api/relay)"), std::string::npos);
  EXPECT_NE(note->find("PP_NODE_REGISTRATION_BASE_URL"), std::string::npos);
  auto id_only = pbr::TryParseObject(R"({ "registration": { "relay_id": "relay:mine" } })");
  EXPECT_TRUE(pbr::IgnoredRegistrationNote(*id_only, loaded, "prod").has_value());
}

TEST(NodeConfigTest, ARegistrationThatRunsOrNoneIsNotSaid) {
  pbr::NodeConfig loaded;
  loaded.registration.base_url = "https://mine.example.test/api/relay";
  auto same = pbr::TryParseObject(R"({ "registration": { "base_url": "https://mine.example.test/api/relay" } })");
  EXPECT_FALSE(pbr::IgnoredRegistrationNote(*same, loaded, "custom").has_value()) << "a custom backend runs the file's";
  auto none = pbr::TryParseObject(R"({ "mesh": {} })");
  EXPECT_FALSE(pbr::IgnoredRegistrationNote(*none, loaded, "prod").has_value());
  auto empty = pbr::TryParseObject(R"({ "registration": {} })");
  EXPECT_FALSE(pbr::IgnoredRegistrationNote(*empty, loaded, "prod").has_value());
}
