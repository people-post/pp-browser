#include "app/node/NodeConfigLoad.h"
#include "common/ValueJson.h"
#include "foundation/data/Config.h"
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

/** The node's keys as one string, to compare two loads. */
std::string Dump(const pbr::NodeConfig& c) {
  pbr::Object o;
  o.set("mesh", pbr::MeshConfigToObject(c.mesh));
  o.set("registration", pbr::ServiceEndpointToObject(c.registration));
  o.set("data_dir", c.data_dir);
  o.set("relay_content_sha256", c.relay_content_sha256);
  o.set("node", c.node);
  return pbr::DumpJson(o);
}

/** The app's loader, projected to the node's keys: what pp-node read before it had a loader of its own. */
std::string AppLoad(const std::string& path) {
  pbr::ResetActiveBackendForTest();
  Args args(path);
  auto app = pbr::Config::Load(args.argc(), args.data(), pbr::Config::Fallback::None);
  if (!app) {
    return "error: " + app.error().message;
  }
  pbr::NodeConfig node;
  node.mesh = app->mesh;
  node.registration = app->registration;
  node.data_dir = app->data_dir;
  node.relay_content_sha256 = app->relay_content_sha256;
  node.node = app->node;
  return Dump(node);
}

std::string NodeLoad(const std::string& path) {
  pbr::ResetActiveBackendForTest();
  Args args(path);
  auto node = pbr::LoadNodeConfig(args.argc(), args.data());
  return node ? Dump(*node) : "error: " + node.error().message;
}

const char* kFiles[] = {
    R"({})",
    R"({ "config_version": 1 })",
    R"({ "config_version": 2 })",
    R"({ "config_version": "x", "mesh": { "amp_udp_port": 5 } })",
    R"(not json)",
    R"({ "theme": "themes/dark.rcss", "llm": { "model": "m" }, "search": { "provider": "x" } })",
    R"({ "data_dir": "/var/lib/pp-node", "relay": { "content_sha256": true } })",
    R"({ "data_dir": 5, "relay": { "content_sha256": "yes" } })",
    R"({ "relay": 3, "mesh": 7, "registration": [] })",
    R"({ "registration": { "base_url": "https://mine.example.test/api/relay", "relay_id": "relay:mine" } })",
    R"({ "registration": { "relay_id": "relay:only" } })",
    R"({ "mesh": { "amp_udp_port": 443, "mesh_publish": true, "mesh_heartbeat": true,
                   "advertise_multiaddrs": ["/ip4/203.0.113.5/udp/443/adp/2"],
                   "bootstrap_peers": ["/ip4/198.51.100.7/udp/443/adp/2/p2p/QmSeed"],
                   "capabilities": { "circuit_relay": true, "media_relay": false, "dht": true },
                   "capacity": { "links": 100, "group_calls": 4 }, "dht": { "k_bucket_size": 8 } } })",
    R"({ "mesh": { "capabilities": { "dht": true } } })",
    R"({ "node": { "relay": { "enabled": true, "retention_days": 3 } } })",
    R"({ "backend": { "env": "custom" }, "relay": { "base_url": "https://r.example.test/api/relay" },
         "registration": { "base_url": "https://reg.example.test/api/relay", "relay_id": "relay:reg" } })",
    R"({ "backend": { "env": "custom" } })",
    R"({ "backend": { "env": "staging" }, "registration": { "base_url": "https://mine.example.test/api/relay" } })",
    R"({ "backend": { "env": "dev" } })",
    R"({ "backend": { "env": "nonsense" } })",
};

/** Every file in kFiles, and no file, loaded both ways under the build's backends as they stand. */
void ExpectBothLoadersAgree(const std::string& build) {
  EXPECT_EQ(NodeLoad(""), AppLoad("")) << build << ": no file";
  for (const char* json : kFiles) {
    const auto path = WriteConfig(json);
    EXPECT_EQ(NodeLoad(path.string()), AppLoad(path.string())) << build << ": " << json;
    std::filesystem::remove(path);
  }
  const std::string missing = (std::filesystem::temp_directory_path() / "pp-node-config-missing.json").string();
  EXPECT_EQ(NodeLoad(missing), AppLoad(missing)) << build << ": a file that is not there";
}

} // namespace

// capability-repos CR028: pp-node's own loader reads what the app's loader gave it, for every file and build here.
TEST(NodeConfigTest, TheNodesLoaderAgreesWithTheAppsInABuildWithNoBackend) {
  EnvGuard origin("PP_BROWSER_ORIGIN");
  EnvGuard config("PP_BROWSER_CONFIG");
  ExpectBothLoadersAgree("no compiled backend");
  pbr::ResetActiveBackendForTest();
}

TEST(NodeConfigTest, TheNodesLoaderAgreesWithTheAppsInAStoreBuild) {
  EnvGuard origin("PP_BROWSER_ORIGIN");
  EnvGuard config("PP_BROWSER_CONFIG");
  pbr::CompiledOrigins compiled;
  compiled.prod = "https://api.example.test";
  compiled.prod_relay_id = "relay:prod";
  compiled.staging = "https://staging.example.test";
  compiled.staging_relay_id = "relay:staging";
  compiled.kind = pbr::BuildKind::Store;
  pbr::ScopedCompiledOriginsForTest scoped(compiled);
  ExpectBothLoadersAgree("store build");
}

TEST(NodeConfigTest, TheNodesLoaderAgreesWithTheAppsInATesterBuild) {
  EnvGuard origin("PP_BROWSER_ORIGIN");
  EnvGuard config("PP_BROWSER_CONFIG");
  pbr::CompiledOrigins compiled;
  compiled.prod = "https://api.example.test";
  compiled.staging = "https://staging.example.test";
  compiled.staging_relay_id = "relay:staging";
  compiled.dev = "https://dev.example.test";
  compiled.kind = pbr::BuildKind::Tester;
  pbr::ScopedCompiledOriginsForTest scoped(compiled);
  ExpectBothLoadersAgree("tester build");
}

TEST(NodeConfigTest, TheNodesLoaderAgreesWithTheAppsUnderAnOriginOverride) {
  EnvGuard origin("PP_BROWSER_ORIGIN");
  EnvGuard relay_id("PP_BROWSER_RELAY_ID");
  EnvGuard config("PP_BROWSER_CONFIG");
  pbr::CompiledOrigins compiled;
  compiled.prod = "https://api.example.test";
  compiled.prod_relay_id = "relay:prod";
  pbr::ScopedCompiledOriginsForTest scoped(compiled);
  SetEnv("PP_BROWSER_ORIGIN", "https://lab.example.test");
  SetEnv("PP_BROWSER_RELAY_ID", "relay:lab");
  ExpectBothLoadersAgree("PP_BROWSER_ORIGIN");
}

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
