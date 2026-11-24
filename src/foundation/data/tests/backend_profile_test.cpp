#include "foundation/data/BackendProfile.h"
#include "foundation/data/ProfileManifest.h"
#include "foundation/data/ProfileRegistry.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(now))).string();
}

void SetOriginEnv(const char* value) {
#ifdef _WIN32
  _putenv_s("PP_BROWSER_ORIGIN", value ? value : "");
#else
  if (value) {
    setenv("PP_BROWSER_ORIGIN", value, 1);
  } else {
    unsetenv("PP_BROWSER_ORIGIN");
  }
#endif
}

class BackendProfileTest : public ::testing::Test {
protected:
  void SetUp() override {
    pbr::ResetActiveBackendForTest();
    SetOriginEnv(nullptr);
    data_dir_ = UniquePath("pp_browser_backend_profile_");
    std::filesystem::remove_all(data_dir_);
  }
  void TearDown() override {
    pbr::ResetActiveBackendForTest();
    std::filesystem::remove_all(data_dir_);
  }
  std::string data_dir_;
};

// This build's custom key prefix (release-compatibility RC022): a tester build's never names a store build's profile.
std::string CustomPrefix() {
  return pbr::IsTesterBuild() ? "test-custom-" : "custom-";
}

} // namespace

TEST_F(BackendProfileTest, KeysAreStableAndFilesystemSafe) {
  pbr::AppConfig config;
  config.registration.base_url = "https://Custom.Example.test/api/relay/";
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom);
  const std::string key = pbr::BackendProfileKey(config);
  EXPECT_EQ(key.rfind(CustomPrefix(), 0), 0u);
  EXPECT_EQ(key.size(), CustomPrefix().size() + 12);
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(key));

  config.registration.base_url = "https://custom.example.test/api/relay";
  EXPECT_EQ(pbr::BackendProfileKey(config), key) << "case and a trailing slash do not name another backend";
  config.registration.base_url = "https://other.example.test/api/relay";
  EXPECT_NE(pbr::BackendProfileKey(config), key);
}

TEST_F(BackendProfileTest, ProdKeyAndEnvOriginKey) {
  pbr::AppConfig config;
  if (pbr::IsTesterBuild()) {
    EXPECT_NE(pbr::BackendProfileKey(config), "prod") << "a tester build never opens prod's profile";
  } else {
    EXPECT_EQ(pbr::BackendProfileKey(config), "prod");
  }
  SetOriginEnv("https://backend.example.test");
  const std::string key = pbr::BackendProfileKey(config);
  EXPECT_EQ(key.rfind(CustomPrefix(), 0), 0u);
  SetOriginEnv(nullptr);
}

TEST_F(BackendProfileTest, SavedKeyDiffersFromRunningOneOnlyAfterASwitch) {
  pbr::AppConfig config;
  pbr::LatchActiveBackend(pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::SavedBackendKey(config), pbr::BackendProfileKey(config));

  config.backend.env = "custom";
  config.registration.base_url = "https://custom.example.test/api/relay";
  const std::string custom = pbr::SavedBackendKey(config);
  EXPECT_EQ(custom.rfind(CustomPrefix(), 0), 0u);
  EXPECT_NE(custom, pbr::BackendProfileKey(config));

  // Under an origin the loaded URLs are replaced; the saved copies name the custom backend.
  config.registration.base_url = "https://origin.example.test/api/relay";
  config.backend.has_saved_urls = true;
  config.backend.saved_registration.base_url = "https://Custom.Example.test/api/relay/";
  EXPECT_EQ(pbr::SavedBackendKey(config), custom);

  config.backend.env = "prod";
  EXPECT_EQ(pbr::SavedBackendKey(config), pbr::BackendProfileKey(config));
}

TEST_F(BackendProfileTest, SavedKeyIsConstantUnderAnOriginOverride) {
  SetOriginEnv("https://backend.example.test");
  pbr::AppConfig config;
  config.backend.env = "custom";
  EXPECT_EQ(pbr::SavedBackendKey(config), pbr::BackendProfileKey(config));
  SetOriginEnv(nullptr);
}

// Release-compatibility RC022: the keys as each kind of build makes them, with the compiled origins passed in.
namespace {

constexpr pbr::CompiledOrigins kStore{.prod = "https://prod.example.test"};
constexpr pbr::CompiledOrigins kTesterAll{
    .staging = "https://staging.example.test", .dev = "https://dev.example.test", .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterDevOnly{.dev = "https://dev.example.test", .kind = pbr::BuildKind::Tester};
constexpr pbr::CompiledOrigins kTesterNone{.kind = pbr::BuildKind::Tester};
constexpr pbr::BackendEnv kAllEnvs[] = {pbr::BackendEnv::Prod, pbr::BackendEnv::Staging, pbr::BackendEnv::Dev,
                                        pbr::BackendEnv::Custom};

} // namespace

TEST(BackendProfileKeyTest, ACustomBackendIsSeparatePerBuildKindWithTheSameHash) {
  const std::string url = "https://Custom.Example.test/api/relay/";
  const std::string store = pbr::CustomBackendId(url, "", "", pbr::BuildKind::Store);
  const std::string tester = pbr::CustomBackendId(url, "", "", pbr::BuildKind::Tester);
  EXPECT_EQ(store.rfind("custom-", 0), 0u);
  EXPECT_EQ(store.size(), 19u);
  EXPECT_EQ(tester.rfind("test-custom-", 0), 0u);
  EXPECT_EQ(tester.size(), 24u);
  EXPECT_EQ(tester.substr(12), store.substr(7)) << "the same hash";
  EXPECT_NE(tester, store);
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(store));
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(tester));
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Custom, url, "", "", kStore), store);
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Custom, url, "", "", kTesterAll), tester);
}

TEST(BackendProfileKeyTest, ATesterBuildNeverYieldsProdsKey) {
  for (const pbr::CompiledOrigins& compiled : {kTesterAll, kTesterDevOnly, kTesterNone}) {
    for (const pbr::BackendEnv env : kAllEnvs) {
      const std::string key = pbr::BackendIdFor(env, "https://custom.example.test/api/relay", "", "", compiled);
      EXPECT_NE(key, "prod");
      EXPECT_NE(key.rfind("custom-", 0), 0u) << key << ": a store build's custom profile";
      EXPECT_TRUE(pbr::ProfileNetworkAllowed(pbr::NetworkOfBackend(key), pbr::BuildKind::Tester)) << key;
    }
  }
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Prod, "", "", "", kTesterAll), "staging");
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Prod, "", "", "", kTesterDevOnly), "dev");
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Staging, "", "", "", kTesterDevOnly), "dev");
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Dev, "", "", "", kTesterAll), "dev");
  // Neither staging nor dev compiled: a saved prod runs the custom backend (its URLs; none: no backend) under a tester
  // profile — never prod's.
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Prod, "", "", "", kTesterNone),
            pbr::CustomBackendId("", "", "", pbr::BuildKind::Tester));
  // A store build, as before.
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Prod, "", "", "", kStore), "prod");
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Staging, "", "", "", kStore), "prod");
}

// A tester build never opens a profile of an environment it cannot use (its relay id pending): a saved staging whose
// setup could not finish opens the first usable environment's profile (release-compatibility RC022).
TEST(BackendProfileKeyTest, ATesterBuildOpensNoProfileOfAnEnvironmentItCannotUse) {
  constexpr pbr::CompiledOrigins kStagingPending{.staging = "https://staging.example.test",
                                                 .dev = "https://dev.example.test",
                                                 .staging_relay_id = pbr::kPendingRelayId,
                                                 .dev_relay_id = "relay:dev",
                                                 .kind = pbr::BuildKind::Tester};
  constexpr pbr::CompiledOrigins kBothPending{.staging = "https://staging.example.test",
                                              .dev = "https://dev.example.test",
                                              .staging_relay_id = pbr::kPendingRelayId,
                                              .dev_relay_id = pbr::kPendingRelayId,
                                              .kind = pbr::BuildKind::Tester};
  for (const pbr::BackendEnv env : {pbr::BackendEnv::Prod, pbr::BackendEnv::Staging, pbr::BackendEnv::Dev}) {
    EXPECT_EQ(pbr::BackendIdFor(env, "", "", "", kStagingPending), "dev") << pbr::BackendEnvName(env);
    EXPECT_EQ(pbr::BackendIdFor(env, "", "", "", kBothPending),
              pbr::CustomBackendId("", "", "", pbr::BuildKind::Tester))
        << pbr::BackendEnvName(env);
  }
  constexpr pbr::CompiledOrigins kStorePending{.prod = "https://prod.example.test",
                                               .staging = "https://staging.example.test",
                                               .prod_relay_id = pbr::kPendingRelayId,
                                               .staging_relay_id = pbr::kPendingRelayId};
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Staging, "", "", "", kStorePending), "staging")
      << "a store build: as before";
  EXPECT_EQ(pbr::BackendIdFor(pbr::BackendEnv::Prod, "", "", "", kStorePending), "prod");
}

TEST(BackendProfileKeyTest, ATesterBuildOpensNoProfileOnTheMainNetwork) {
  for (const char* store_key : {"prod", "custom-0123456789ab"}) {
    EXPECT_EQ(pbr::NetworkOfBackend(store_key), pbr::profile_network::kMain) << store_key;
  }
  EXPECT_EQ(pbr::NetworkOfBackend("staging"), pbr::profile_network::kStaging);
  EXPECT_EQ(pbr::NetworkOfBackend("dev"), pbr::profile_network::kDev);
  EXPECT_EQ(pbr::NetworkOfBackend("test-custom-0123456789ab"), pbr::profile_network::kTestCustom);
  EXPECT_EQ(pbr::NetworkOfBackend("node"), pbr::profile_network::kNode);
  for (const char* unknown : {"", "qa", "default", "custom-", "test-custom-", "main"}) {
    EXPECT_TRUE(pbr::NetworkOfBackend(unknown).empty()) << unknown;
  }

  EXPECT_FALSE(pbr::ProfileNetworkAllowed(pbr::profile_network::kMain, pbr::BuildKind::Tester));
  EXPECT_TRUE(pbr::ProfileNetworkAllowed(pbr::profile_network::kMain, pbr::BuildKind::Store));
  for (const std::string_view both : {pbr::profile_network::kStaging, pbr::profile_network::kDev}) {
    EXPECT_TRUE(pbr::ProfileNetworkAllowed(both, pbr::BuildKind::Tester)) << both;
    EXPECT_TRUE(pbr::ProfileNetworkAllowed(both, pbr::BuildKind::Store)) << both;
  }
  EXPECT_TRUE(pbr::ProfileNetworkAllowed(pbr::profile_network::kTestCustom, pbr::BuildKind::Tester));
  EXPECT_FALSE(pbr::ProfileNetworkAllowed(pbr::profile_network::kTestCustom, pbr::BuildKind::Store));
  for (const pbr::BuildKind kind : {pbr::BuildKind::Store, pbr::BuildKind::Tester}) {
    EXPECT_FALSE(pbr::ProfileNetworkAllowed(pbr::profile_network::kNode, kind)) << "pp-node's alone";
    EXPECT_FALSE(pbr::ProfileNetworkAllowed("", kind));
  }
}

// This build's running key against its kind.
TEST_F(BackendProfileTest, ThisBuildsKeysFollowItsKind) {
  pbr::AppConfig config;
  for (const pbr::BackendEnv env : kAllEnvs) {
    pbr::ResetActiveBackendForTest();
    pbr::LatchActiveBackend(env);
    EXPECT_TRUE(
        pbr::ProfileNetworkAllowed(pbr::NetworkOfBackend(pbr::BackendProfileKey(config)), pbr::ThisBuildKind()));
    config.backend.env = pbr::BackendEnvName(env);
    EXPECT_TRUE(pbr::ProfileNetworkAllowed(pbr::NetworkOfBackend(pbr::SavedBackendKey(config)), pbr::ThisBuildKind()));
  }
}

// profile-model P4 (#745, #747): one rule for the restart banner and push — the saved config selects another backend id
// than the run's. A custom backend's registration URL or org relay edited is one; any other service edited is not.
TEST_F(BackendProfileTest, ASwitchIsPendingWhenTheSavedConfigSelectsAnotherBackendId) {
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom);
  pbr::AppConfig config;
  config.backend.env = "custom";
  config.registration.base_url = "https://custom.example.test/api/relay";
  config.relay.base_url = "https://custom.example.test/api/relay";
  config.relay.relay_id = "relay:custom";
  const std::string running = pbr::BackendProfileKey(config);
  EXPECT_FALSE(pbr::BackendSwitchPending(config, running));

  pbr::AppConfig llm = config;
  llm.llm.base_url = "https://llm.example.test/v1";
  EXPECT_FALSE(pbr::BackendSwitchPending(llm, running)) << "not part of the id";

  pbr::AppConfig registration = config;
  registration.registration.base_url = "https://other.example.test/api/relay";
  EXPECT_TRUE(pbr::BackendSwitchPending(registration, running));

  pbr::AppConfig relay = config;
  relay.relay.relay_id = "relay:other";
  EXPECT_TRUE(pbr::BackendSwitchPending(relay, running));

  pbr::AppConfig other_env = config;
  other_env.backend.env = pbr::IsTesterBuild() ? "staging" : "prod";
  EXPECT_TRUE(pbr::BackendSwitchPending(other_env, running));
}
