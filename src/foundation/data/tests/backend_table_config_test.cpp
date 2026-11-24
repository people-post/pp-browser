// Backends as service tables, through the config pipeline (profile-model P2): what a config file does to the active table
// and to the AppConfig every caller reads — built-in backends keep today's URLs, a custom backend reaches only what it
// names, and nothing is taken from another backend. Example hosts only.
#include "common/ValueJson.h"
#include "foundation/data/BackendProfile.h"
#include "foundation/data/Config.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using pp::common::Object;

constexpr pbr::CompiledOrigins kCompiled{.prod = "https://prod.example.test",
                                         .staging = "https://staging.example.test",
                                         .dev = "https://dev.example.test",
                                         .prod_relay_id = "relay:prod",
                                         .staging_relay_id = "relay:staging",
                                         .dev_relay_id = "relay:dev",
                                         .prod_relay_certificate = "cHJvZA==",
                                         .staging_relay_certificate = "c3RhZ2luZw==",
                                         .dev_relay_certificate = "ZGV2"};

class ScopedEnv {
public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name)) {
      had_ = true;
      old_ = old;
    }
    Set(value);
  }
  ~ScopedEnv() { Set(had_ ? old_.c_str() : nullptr); }

private:
  void Set(const char* value) const {
#ifdef _WIN32
    _putenv_s(name_.c_str(), value ? value : "");
#else
    if (value) {
      setenv(name_.c_str(), value, 1);
    } else {
      unsetenv(name_.c_str());
    }
#endif
  }
  std::string name_;
  bool had_ = false;
  std::string old_;
};

/** A run of the config pipeline against `kCompiled`: no PP_BROWSER_ORIGIN, a fresh latch. */
class BackendTableConfigTest : public testing::Test {
protected:
  std::string Write(const std::string& name, const std::string& body) const {
    const std::string path = (std::filesystem::temp_directory_path() / ("pp_backend_table_" + name + ".json")).string();
    std::ofstream out(path);
    out << body;
    written_.push_back(path);
    return path;
  }
  pbr::AppConfig Load(const std::string& name, const std::string& body) const {
    auto loaded = pbr::Config::LoadFromFile(Write(name, body), false);
    EXPECT_TRUE(static_cast<bool>(loaded)) << (loaded ? "" : loaded.error().message);
    return loaded ? *loaded : pbr::AppConfig{};
  }
  ~BackendTableConfigTest() override {
    for (const std::string& path : written_) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  ScopedEnv origin_{"PP_BROWSER_ORIGIN", nullptr};
  ScopedEnv relay_id_{"PP_BROWSER_RELAY_ID", nullptr};
  pbr::ScopedCompiledOriginsForTest compiled_{kCompiled};
  mutable std::vector<std::string> written_;
};

/** A custom backend's file naming every service, except `without`. */
std::string CustomFile(const std::string& without = "") {
  const auto line = [&without](const std::string& key, const std::string& json) {
    return key == without ? std::string() : json;
  };
  std::string services = "\"backend\": { \"env\": \"custom\", \"services\": {";
  const std::vector<std::pair<std::string, std::string>> rows = {
      {"llm_gateway", "https://custom.example.test/api/llm/v1"},
      {"llm_token", "https://custom.example.test/api/llm/v1"},
      {"promoted_mcp", "https://custom.example.test/mcp"},
      {"crash_intake", "https://custom.example.test/api/relay"},
      {"account_locker", "https://custom.example.test/api/relay"},
      {"client_compat", "https://custom.example.test/api/relay"}};
  bool first = true;
  for (const auto& [key, value] : rows) {
    if (key == without) {
      continue;
    }
    services += std::string(first ? "" : ",") + " \"" + key + "\": \"" + value + "\"";
    first = false;
  }
  services += " } }";
  return "{ \"config_version\": 1, " + services +
         ", \"llm\": { \"preset\": \"brief\" },"
         " \"relay\": { \"base_url\": \"https://custom.example.test/api/relay\", \"relay_id\": \"relay:custom\" },"
         " \"registration\": { \"base_url\": \"https://custom.example.test/api/relay\", \"relay_id\": \"relay:custom\" "
         "},"
         " \"directory\": " +
         line("directory", "{ \"base_url\": \"https://custom.example.test/api/relay\" }") +
         (without == "directory" ? "{}" : "") + " }";
}

bool NamesAnotherBackend(const std::string& address) {
  return address.find("prod.example.test") != std::string::npos ||
         address.find("staging.example.test") != std::string::npos ||
         address.find("dev.example.test") != std::string::npos;
}

std::vector<std::string> AddressesOf(const pbr::AppConfig& config) {
  return {config.llm.base_url, config.relay.base_url, config.directory.base_url, config.registration.base_url,
          config.promoted_mcp.url};
}

} // namespace

// Parity of the config pipeline for the built-in backends: prod, staging and dev still resolve to what the one origin gave
// them. Recorded from the behaviour before the tables (ApplyBackendOverride): every URL under the origin, the build's
// relay id on both relay-shaped endpoints. The promoted MCP is the seed now (#744), the same address.
TEST_F(BackendTableConfigTest, BuiltInBackendsKeepTodaysResolvedUrls) {
  struct Expect {
    const char* env;
    const char* origin;
    const char* relay_id;
  };
  const Expect rows[] = {{"prod", "https://prod.example.test", "relay:prod"},
                         {"staging", "https://staging.example.test", "relay:staging"},
                         {"dev", "https://dev.example.test", "relay:dev"}};
  for (const Expect& row : rows) {
    SCOPED_TRACE(row.env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config =
        Load(std::string("builtin_") + row.env,
             std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + row.env + "\" } }");
    const std::string origin = row.origin;
    EXPECT_EQ(config.backend.env, row.env);
    EXPECT_EQ(config.llm.base_url, origin + "/api/llm/v1");
    EXPECT_EQ(config.relay.base_url, origin + "/api/relay");
    EXPECT_EQ(config.directory.base_url, origin + "/api/relay");
    EXPECT_TRUE(config.directory.providers.empty());
    EXPECT_EQ(config.registration.base_url, origin + "/api/relay");
    EXPECT_EQ(config.promoted_mcp.url, origin + "/mcp");
    EXPECT_EQ(config.relay.relay_id, row.relay_id);
    EXPECT_EQ(config.registration.relay_id, row.relay_id);
    EXPECT_TRUE(config.backend.has_saved_urls);
    const pbr::BackendTable table = pbr::ActiveBackendTable();
    EXPECT_EQ(table.id, row.env);
    EXPECT_EQ(table.llm_token_url, origin + "/api/llm/v1");
    EXPECT_EQ(table.account_locker_url, origin + "/api/relay");
    EXPECT_EQ(table.client_compat_url, origin + "/api/relay");
    EXPECT_EQ(table.crash_intake_url, origin + "/api/relay");
    EXPECT_EQ(pbr::BriefOrgRelayIdentity().relay_id, row.relay_id);
  }
}

// #741: take each service in turn out of a custom table. The feature's address is gone, every other address is the custom
// backend's own, and nothing — LLM, MCP, directory, the lot — names a built-in backend (the build's prod, staging and dev
// are all compiled here, so any fallback would show).
TEST_F(BackendTableConfigTest, ARemovedServiceIsAbsentAndNothingFallsBackToAnotherBackend) {
  const char* const services[] = {"llm_gateway",  "llm_token",      "directory",    "promoted_mcp",
                                  "crash_intake", "account_locker", "client_compat"};
  for (const char* removed : services) {
    SCOPED_TRACE(removed);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config = Load(std::string("custom_without_") + removed, CustomFile(removed));
    const pbr::BackendTable table = pbr::ActiveBackendTable();
    EXPECT_EQ(config.backend.env, "custom");
    EXPECT_EQ(table.id, "custom");
    const std::string gone = removed;
    EXPECT_EQ(table.llm_gateway_url.empty(), gone == "llm_gateway");
    EXPECT_EQ(table.llm_token_url.empty(), gone == "llm_token");
    EXPECT_EQ(table.directory_providers.empty(), gone == "directory");
    EXPECT_EQ(table.promoted_mcp_url.empty(), gone == "promoted_mcp");
    EXPECT_EQ(table.crash_intake_url.empty(), gone == "crash_intake");
    EXPECT_EQ(table.account_locker_url.empty(), gone == "account_locker");
    EXPECT_EQ(table.client_compat_url.empty(), gone == "client_compat");
    // What every caller reads (AppConfig and the accessors) says the same.
    EXPECT_EQ(config.llm.base_url.empty(), gone == "llm_gateway");
    EXPECT_EQ(pbr::BriefLlmBaseUrl().empty(), gone == "llm_gateway");
    EXPECT_EQ(config.directory.base_url.empty(), gone == "directory");
    EXPECT_EQ(config.promoted_mcp.url.empty(), gone == "promoted_mcp");
    EXPECT_EQ(pbr::BriefMcpUrl().empty(), gone == "promoted_mcp");
    for (const std::string& address : AddressesOf(config)) {
      EXPECT_FALSE(NamesAnotherBackend(address)) << address;
    }
    EXPECT_FALSE(NamesAnotherBackend(table.llm_gateway_url + table.llm_token_url + table.crash_intake_url +
                                     table.account_locker_url + table.client_compat_url));
  }
}

// #742: a custom relay or registration without `relay_id` names none — not the build's, which is compiled here.
TEST_F(BackendTableConfigTest, ACustomRelayOrRegistrationWithoutARelayIdNamesNone) {
  const pbr::AppConfig config = Load("custom_no_relay_id",
                                     R"({ "config_version": 1, "backend": { "env": "custom" },
           "relay": { "base_url": "https://custom.example.test/api/relay" },
           "registration": { "base_url": "https://custom.example.test/api/relay" },
           "directory": { "base_url": "https://custom.example.test/api/relay" } })");
  EXPECT_TRUE(config.relay.relay_id.empty());
  EXPECT_TRUE(config.registration.relay_id.empty());
  EXPECT_TRUE(pbr::BriefRelayId().empty());
  EXPECT_TRUE(pbr::BriefOrgRelayIdentity().certificate_b64.empty());
  EXPECT_EQ(config.relay.base_url, "https://custom.example.test/api/relay");
  // Named, it is kept.
  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig named = Load("custom_relay_id", CustomFile());
  EXPECT_EQ(named.relay.relay_id, "relay:custom");
  EXPECT_EQ(named.registration.relay_id, "relay:custom");
}

// A custom backend that names nothing is empty: no default of the fallback environment fills a field in.
TEST_F(BackendTableConfigTest, ACustomConfigThatNamesNothingHasNoBackendAddressAtAll) {
  const pbr::AppConfig config = Load("custom_empty", R"({ "config_version": 1, "backend": { "env": "custom" } })");
  for (const std::string& address : AddressesOf(config)) {
    EXPECT_TRUE(address.empty()) << address;
  }
  EXPECT_TRUE(config.relay.relay_id.empty());
  EXPECT_TRUE(pbr::ActiveBackendTable().directory_providers.empty());
}

// #743: a `directory.providers` list in config.json does not steer a built-in backend's directory.
TEST_F(BackendTableConfigTest, DirectoryProvidersInTheConfigCannotOverrideABuiltInBackendsDirectory) {
  for (const char* env : {"prod", "staging", "dev"}) {
    SCOPED_TRACE(env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config =
        Load(std::string("providers_") + env, std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + env +
                                                  "\" }, \"directory\": { \"providers\": [ { \"base_url\": "
                                                  "\"https://elsewhere.example.test/api/relay\" } ] } }");
    const std::vector<pbr::ServiceEndpointConfig> effective = pbr::EffectiveDirectoryProviders(config.directory);
    ASSERT_EQ(effective.size(), 1U);
    EXPECT_EQ(effective.front().base_url, pbr::ActiveBackendTable().DirectoryUrl());
    EXPECT_NE(effective.front().base_url.find(std::string(env) + ".example.test"), std::string::npos);
    // The file's list is kept for a save (a later switch to a custom backend finds it), only ignored while it runs.
    ASSERT_EQ(config.backend.saved_directory.providers.size(), 1U);
    EXPECT_EQ(config.backend.saved_directory.providers.front().base_url, "https://elsewhere.example.test/api/relay");
  }
}

// #744: the user's promoted-MCP address is not overwritten on a start and not written back derived.
TEST_F(BackendTableConfigTest, AUsersPromotedMcpUrlSurvivesAStartOnProdStagingAndDev) {
  for (const char* env : {"prod", "staging", "dev"}) {
    SCOPED_TRACE(env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config =
        Load(std::string("mcp_user_") + env,
             std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + env +
                 "\" }, \"promoted_mcp\": { \"url\": \"https://mine.example.test/mcp\", \"set_by_user\": true } }");
    EXPECT_EQ(config.promoted_mcp.url, "https://mine.example.test/mcp") << "not replaced by the backend's";
    // ...and a save writes the user's, not a derived one.
    const Object saved = pbr::ConfigToObject(config, pbr::Config::kConfigVersion);
    const Object* mcp = saved.getObject("promoted_mcp");
    ASSERT_NE(mcp, nullptr);
    EXPECT_EQ(mcp->getString("url").value_or(""), "https://mine.example.test/mcp");
    EXPECT_TRUE(mcp->getIf<bool>("set_by_user").value_or(false)) << "written back with the marker";
  }
}

TEST_F(BackendTableConfigTest, TheBackendsOwnPromotedMcpSeedIsNeverWrittenBack) {
  for (const char* env : {"prod", "staging", "dev"}) {
    SCOPED_TRACE(env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config =
        Load(std::string("mcp_seed_") + env,
             std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + env + "\" } }");
    EXPECT_EQ(config.promoted_mcp.url, pbr::ActiveBackendTable().promoted_mcp_url)
        << "the profile starts with the seed";
    EXPECT_EQ(config.promoted_mcp.url, std::string("https://") + env + ".example.test/mcp");
    const Object saved = pbr::ConfigToObject(config, pbr::Config::kConfigVersion);
    EXPECT_EQ(saved.getObject("promoted_mcp"), nullptr) << "a derived address is not the user's: not saved";
  }
}

// `config_version` stays 1 and the new keys are optional: a file without them reads and writes as before, and the
// custom services a file names come back out of a save.
TEST_F(BackendTableConfigTest, TheNewConfigKeysAreOptionalAndRoundTrip) {
  EXPECT_EQ(pbr::Config::kConfigVersion, 1);
  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig plain = Load("plain", R"({ "config_version": 1, "backend": { "env": "prod" } })");
  const Object plain_saved = pbr::ConfigToObject(plain, pbr::Config::kConfigVersion);
  const Object* plain_backend = plain_saved.getObject("backend");
  ASSERT_NE(plain_backend, nullptr);
  EXPECT_EQ(plain_backend->getObject("services"), nullptr) << "no services named: no key written";
  EXPECT_EQ(plain_saved.getIf<int64_t>("config_version").value_or(0), 1);

  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig custom = Load("custom_roundtrip", CustomFile());
  const Object saved = pbr::ConfigToObject(custom, pbr::Config::kConfigVersion);
  const Object* services = saved.getObject("backend")->getObject("services");
  ASSERT_NE(services, nullptr);
  EXPECT_EQ(services->getString("llm_gateway").value_or(""), "https://custom.example.test/api/llm/v1");
  EXPECT_EQ(services->getString("crash_intake").value_or(""), "https://custom.example.test/api/relay");
  const pbr::CustomServices reread = pbr::CustomServicesFromObject(saved);
  EXPECT_EQ(reread.account_locker_url, "https://custom.example.test/api/relay");
  EXPECT_EQ(reread.org_relay_id, "relay:custom");
  ASSERT_EQ(reread.directory_providers.size(), 1U);
}

// PM010: the custom backend's id is its registration URL and its org relay (origin and id); the function exists and is
// tested, but profile selection has not moved to it.
TEST(BackendIdTest, ACustomBackendIdIsStableUnderEditsOfTheOtherServices) {
  const std::string id = pbr::CustomBackendId("https://reg.example.test/api/relay",
                                              "https://org.example.test/api/relay", "relay:org", pbr::BuildKind::Store);
  EXPECT_EQ(id.rfind("custom-", 0), 0U);
  EXPECT_EQ(id.size(), std::string("custom-").size() + 12);
  // Case of the host, a trailing slash, a different path on the org relay, spaces around the relay id: the same backend.
  EXPECT_EQ(pbr::CustomBackendId("HTTPS://Reg.example.test/api/relay/", "https://ORG.example.test/other/path",
                                 " relay:org ", pbr::BuildKind::Store),
            id);
  // The other services are not part of it: the table's id follows only the three identifying fields.
  pbr::CustomServices services;
  services.registration_url = "https://reg.example.test/api/relay";
  services.org_relay_url = "https://org.example.test/api/relay";
  services.org_relay_id = "relay:org";
  const std::string before =
      pbr::BackendTableId(pbr::CustomBackendTable(services, pbr::BuildKind::Store), pbr::BuildKind::Store);
  EXPECT_EQ(before, id);
  services.account_locker_url = "https://locker.example.test";
  services.client_compat_url = "https://compat.example.test";
  services.llm_gateway_url = "https://llm.example.test/api/llm/v1";
  services.llm_token_url = "https://llm.example.test/api/llm/v1";
  services.crash_intake_url = "https://crash.example.test";
  services.directory_providers = {"https://directory.example.test"};
  services.promoted_mcp_url = "https://mcp.example.test/mcp";
  services.mesh_nodes_url = "https://nodes.example.test";
  services.seed_relays = {"https://extra.example.test/api/relay"};
  EXPECT_EQ(pbr::BackendTableId(pbr::CustomBackendTable(services, pbr::BuildKind::Store), pbr::BuildKind::Store), id);
}

TEST(BackendIdTest, ACustomBackendIdChangesWithTheRegistrationUrlOrTheOrgRelay) {
  const std::string id = pbr::CustomBackendId("https://reg.example.test/api/relay",
                                              "https://org.example.test/api/relay", "relay:org", pbr::BuildKind::Store);
  EXPECT_NE(pbr::CustomBackendId("https://reg2.example.test/api/relay", "https://org.example.test/api/relay",
                                 "relay:org", pbr::BuildKind::Store),
            id);
  EXPECT_NE(pbr::CustomBackendId("https://reg.example.test/api/relay", "https://org2.example.test/api/relay",
                                 "relay:org", pbr::BuildKind::Store),
            id);
  EXPECT_NE(pbr::CustomBackendId("https://reg.example.test/api/relay", "https://org.example.test/api/relay",
                                 "relay:other", pbr::BuildKind::Store),
            id);
  // The fields are not interchangeable: the same strings in other places make another backend.
  EXPECT_NE(pbr::CustomBackendId("https://org.example.test/api/relay", "https://reg.example.test/api/relay",
                                 "relay:org", pbr::BuildKind::Store),
            id);
  // A tester app's custom backends are another network's.
  const std::string tester = pbr::CustomBackendId(
      "https://reg.example.test/api/relay", "https://org.example.test/api/relay", "relay:org", pbr::BuildKind::Tester);
  EXPECT_EQ(tester.rfind("test-custom-", 0), 0U);
  EXPECT_NE(tester, id) << "the prefix names the network, as CustomProfileKey's does";
  // Built-in tables keep their own ids.
  EXPECT_EQ(pbr::BackendTableId(pbr::BuiltInBackendTable(pbr::BackendEnv::Staging, kCompiled), pbr::BuildKind::Store),
            "staging");
}

// Not enabled: the profile folder an install opens is still named by BackendProfileKey / CustomProfileKey (the
// registration URL alone). The PM010 id is another text for the same backend, so using it would move every custom install.
TEST_F(BackendTableConfigTest, TheProfileSelectionKeyIsStillTheRegistrationUrlHashNotThePm010Id) {
  const pbr::AppConfig config = Load("selection_key", CustomFile());
  const std::string key = pbr::BackendProfileKey(config);
  EXPECT_EQ(key, pbr::CustomProfileKey("https://custom.example.test/api/relay", pbr::ThisBuildKind()));
  const std::string pm010 = pbr::BackendTableId(pbr::ActiveBackendTable(), pbr::ThisBuildKind());
  EXPECT_EQ(pm010, pbr::CustomBackendId("https://custom.example.test/api/relay",
                                        "https://custom.example.test/api/relay", "relay:custom", pbr::ThisBuildKind()));
  EXPECT_NE(key, pm010) << "different inputs, different text: the selection must not have moved to it";
  // The same config, edited in a service that is not part of either id, keeps the selection key.
  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig edited = Load("selection_key_edited", CustomFile("crash_intake"));
  EXPECT_EQ(pbr::BackendProfileKey(edited), key);
  // Built-in backends keep their plain keys.
  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig prod = Load("selection_prod", R"({ "config_version": 1, "backend": { "env": "prod" } })");
  EXPECT_EQ(pbr::BackendProfileKey(prod), "prod");
}

// ---- The promoted MCP in files the old app wrote (#741 MCP half, #744) -------------------------------------------------
// The old app wrote the running backend's derived `/mcp` into every config.json it saved. That address was never the
// user's: any built-in backend's is a seed, replaced by the active backend's own (none on a custom backend naming none).

// A store build's custom backend, its file still holding prod's seed: no MCP, certainly not prod's.
TEST_F(BackendTableConfigTest, AStoreCustomFileHoldingProdsMcpSeedCallsNoMcpAtAll) {
  const pbr::AppConfig config = Load("mcp_old_custom_none", R"({ "config_version": 1, "backend": { "env": "custom" },
      "relay": { "base_url": "https://custom.example.test/api/relay" },
      "promoted_mcp": { "url": "https://prod.example.test/mcp" } })");
  EXPECT_FALSE(config.promoted_mcp.IsConfigured());
  EXPECT_FALSE(pbr::ResolvePromotedMcp(config, pbr::Config::DefaultAppConfig(false)).IsConfigured());
  EXPECT_TRUE(pbr::BriefMcpUrl().empty());
  EXPECT_EQ(pbr::ConfigToObject(config, 1).getObject("promoted_mcp"), nullptr) << "and nothing is written back";
  // The same file, the custom table naming its own feed: that one.
  pbr::ResetActiveBackendForTest();
  const pbr::AppConfig named = Load("mcp_old_custom_named", R"({ "config_version": 1, "backend": { "env": "custom",
      "services": { "promoted_mcp": "https://custom.example.test/mcp" } },
      "promoted_mcp": { "url": "https://PROD.example.test/mcp/" } })");
  EXPECT_EQ(named.promoted_mcp.url, "https://custom.example.test/mcp") << "compared normalized";
}

// A file saved by a store prod build, read by a tester build that has NO prod origin compiled (CMake refuses one): the
// leftover is recognised by having no marker, not by comparing with compiled origins, so the tester uses its own seed.
TEST(BackendTableMcpSeedTest, ATesterBuildWithNoProdOriginReadingAStoreProdFileGetsItsOwnEnvironmentsSeed) {
  const ScopedEnv no_origin("PP_BROWSER_ORIGIN", nullptr);
  constexpr pbr::CompiledOrigins tester{.staging = "https://staging.example.test",
                                        .dev = "https://dev.example.test",
                                        .staging_relay_id = "relay:staging",
                                        .dev_relay_id = "relay:dev",
                                        .kind = pbr::BuildKind::Tester};
  const pbr::ScopedCompiledOriginsForTest compiled(tester);
  const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_table_tester_mcp.json").string();
  {
    std::ofstream out(path);
    out << R"({ "config_version": 1, "backend": { "env": "prod" },
                "promoted_mcp": { "url": "https://store-prod.example.test/mcp" } })";
  }
  auto loaded = pbr::Config::LoadFromFile(path, false);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, "staging") << "a tester build never runs prod";
  EXPECT_EQ(loaded->promoted_mcp.url, "https://staging.example.test/mcp");
  EXPECT_EQ(pbr::ConfigToObject(*loaded, 1).getObject("promoted_mcp"), nullptr);
}

// The backend was switched in Settings (saved) before the upgrade: the file says staging and holds prod's old seed.
TEST_F(BackendTableConfigTest, ASwitchSavedWithoutARestartThenAnUpgradeGetsTheNewBackendsSeed) {
  const pbr::AppConfig config = Load("mcp_switched", R"({ "config_version": 1, "backend": { "env": "staging" },
      "promoted_mcp": { "url": "https://prod.example.test/mcp" } })");
  EXPECT_EQ(config.promoted_mcp.url, "https://staging.example.test/mcp");
}

// The marker decides (the old app could write a user's value only where it survived a start, and left no way to tell):
// a marked entry is the user's on every backend, whatever its address, id or switches, and is saved whole with the marker.
TEST_F(BackendTableConfigTest, AMarkedPromotedMcpEntryIsTheUsersOnEveryBackendAndSavedWhole) {
  for (const char* env : {"prod", "staging", "dev", "custom"}) {
    SCOPED_TRACE(env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig off =
        Load(std::string("mcp_marked_") + env,
             std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + env +
                 "\" }, \"promoted_mcp\": { \"id\": \"feeds\", \"enabled\": false, \"set_by_user\": true,"
                 " \"url\": \"https://prod.example.test/mcp\" } }");
    EXPECT_EQ(off.promoted_mcp.id, "feeds");
    EXPECT_FALSE(off.promoted_mcp.enabled);
    EXPECT_EQ(off.promoted_mcp.url, "https://prod.example.test/mcp") << "the user's, even if it looks like a seed";
    const Object saved = pbr::ConfigToObject(off, 1);
    const Object* mcp = saved.getObject("promoted_mcp");
    ASSERT_NE(mcp, nullptr);
    EXPECT_EQ(mcp->getString("id").value_or(""), "feeds");
    EXPECT_FALSE(mcp->getIf<bool>("enabled").value_or(true));
    EXPECT_TRUE(mcp->getIf<bool>("set_by_user").value_or(false));
    // Round trip: what a save writes reads back as the same entry.
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig again = Load(std::string("mcp_marked_again_") + env, pp::common::DumpJson(saved, 2));
    EXPECT_EQ(again.promoted_mcp.url, off.promoted_mcp.url);
    EXPECT_TRUE(again.promoted_mcp.user_set);
  }
}

// Any other entry is a leftover of the old app on every backend, whatever its id or switches. Decision: on a custom backend
// main kept a value typed in Integrations (Config.cpp:156 skipped the override), and that one is dropped once and must be
// typed again — accepted (unreleased, test data may be discarded; custom backends are developers' only).
TEST_F(BackendTableConfigTest, AnUnmarkedPromotedMcpEntryIsALeftoverOnEveryBackend) {
  for (const char* env : {"prod", "staging", "dev", "custom"}) {
    SCOPED_TRACE(env);
    for (const char* entry : {R"({ "url": "https://mine.example.test/mcp" })",
                              R"({ "id": "feeds", "enabled": false, "url": "https://mine.example.test/mcp" })"}) {
      pbr::ResetActiveBackendForTest();
      const pbr::AppConfig config =
          Load(std::string("mcp_unmarked_") + env, std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") +
                                                       env + "\" }, \"promoted_mcp\": " + entry + " }");
      EXPECT_EQ(config.promoted_mcp.url, pbr::ActiveBackendTable().promoted_mcp_url) << entry;
      EXPECT_NE(config.promoted_mcp.url, "https://mine.example.test/mcp");
      EXPECT_TRUE(config.promoted_mcp.id.empty());
      EXPECT_TRUE(config.promoted_mcp.enabled);
      EXPECT_EQ(pbr::ConfigToObject(config, 1).getObject("promoted_mcp"), nullptr)
          << "a leftover is never written back";
    }
  }
}

// A stdio server the user configured by hand (main kept those: it replaced only the url) is the user's, marked or not, and
// gets the marker on the next save.
TEST_F(BackendTableConfigTest, AnUnmarkedCommandEntryIsTheUsersAndIsMarkedOnSave) {
  for (const char* env : {"prod", "custom"}) {
    SCOPED_TRACE(env);
    pbr::ResetActiveBackendForTest();
    const pbr::AppConfig config =
        Load(std::string("mcp_command_") + env,
             std::string("{ \"config_version\": 1, \"backend\": { \"env\": \"") + env +
                 "\" }, \"promoted_mcp\": { \"command\": \"/usr/bin/feeds\", \"args\": [\"--x\"] } }");
    EXPECT_EQ(config.promoted_mcp.command, "/usr/bin/feeds");
    const Object saved = pbr::ConfigToObject(config, 1);
    const Object* mcp = saved.getObject("promoted_mcp");
    ASSERT_NE(mcp, nullptr);
    EXPECT_EQ(mcp->getString("command").value_or(""), "/usr/bin/feeds");
    EXPECT_TRUE(mcp->getIf<bool>("set_by_user").value_or(false));
  }
}

// An older build reads a file with the marker: `McpConfigFromObject` (ConfigJson.cpp) reads only id, command, url, args and
// enabled, so the unknown key is ignored and the entry loads as before. Held here as the new reader's own tolerance of
// unknown keys in the same object.
TEST(BackendTableMcpSeedTest, UnknownKeysInThePromotedMcpObjectAreIgnored) {
  pbr::McpConfig mcp;
  pbr::McpConfigFromObject(*pp::common::TryParseObject(R"({ "url": "https://x.example.test/mcp", "future_key": 1 })"),
                           mcp);
  EXPECT_EQ(mcp.url, "https://x.example.test/mcp");
  EXPECT_FALSE(mcp.user_set);
}

// ---- Profile folders do not move (this PR must not change which profile any install opens) ----------------------------
// A custom backend whose file names no registration URL used to get the fallback environment's relay filled in before the
// key was made: `custom-<hash of prod's /api/relay>` in a store build. The recorded value of the old behaviour.
TEST_F(BackendTableConfigTest, ACustomInstallWithNoRegistrationUrlKeepsTheProfileFolderItOpenedBefore) {
  const pbr::AppConfig config =
      Load("key_no_registration", R"({ "config_version": 1, "backend": { "env": "custom" } })");
  EXPECT_TRUE(config.registration.base_url.empty());
  const std::string before = pbr::CustomProfileKey("https://prod.example.test/api/relay", pbr::BuildKind::Store);
  EXPECT_EQ(pbr::BackendProfileKey(config), before);
  EXPECT_EQ(pbr::SavedBackendKey(config), before);
  EXPECT_NE(before, pbr::CustomProfileKey("", pbr::BuildKind::Store)) << "not the empty URL's hash";
}

TEST(BackendProfileKeyTest, ATesterBuildsCustomInstallWithNoRegistrationUrlUsesTheTesterEnvironmentsRelay) {
  const ScopedEnv no_origin("PP_BROWSER_ORIGIN", nullptr);
  constexpr pbr::CompiledOrigins tester{
      .staging = "https://staging.example.test", .staging_relay_id = "relay:staging", .kind = pbr::BuildKind::Tester};
  const pbr::ScopedCompiledOriginsForTest compiled(tester);
  const std::string path = (std::filesystem::temp_directory_path() / "pp_backend_table_tester_key.json").string();
  {
    std::ofstream out(path);
    out << R"({ "config_version": 1, "backend": { "env": "custom" } })";
  }
  auto loaded = pbr::Config::LoadFromFile(path, false);
  std::filesystem::remove(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(pbr::BackendProfileKey(*loaded),
            pbr::CustomProfileKey("https://staging.example.test/api/relay", pbr::BuildKind::Tester));
}
