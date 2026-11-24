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

// The active profile with no backend recorded (a fresh install's default, or one just reset, whose manifest is written
// again without one) is the running backend's: staging as much as prod.
TEST_F(BackendProfileTest, AnActiveProfileWithoutABackendTakesTheRunningOne) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string dir = registry->ActiveProfileDataDir();
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(dir))); // a manifest with no backend
  EXPECT_TRUE(pbr::ReadProfileBackend(dir).empty());

  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(registry->ActiveProfileDataDir(), dir);
  EXPECT_EQ(pbr::ReadProfileBackend(dir), "staging");
}

TEST_F(BackendProfileTest, EachBackendGetsItsOwnProfileAndSwitchingBackFindsIt) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string staging_dir = registry->ActiveProfileDataDir();

  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  EXPECT_EQ(registry->ActiveProfileId(), "default") << "a fresh install's default is the first backend's";
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  EXPECT_EQ(registry->ActiveProfileId(), "prod");
  EXPECT_EQ(pbr::ReadProfileBackend(staging_dir), "staging");
  EXPECT_EQ(pbr::ReadProfileBackend(registry->ActiveProfileDataDir()), "prod");

  // Switching back finds the first profile instead of creating another; the choice persists.
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  auto reloaded = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->ActiveProfileId(), "default");
  EXPECT_EQ(reloaded->Profiles().size(), 2u);

  ASSERT_TRUE(static_cast<bool>(reloaded->SelectProfileForBackend("prod")));
  EXPECT_EQ(reloaded->ActiveProfileId(), "prod");
  EXPECT_EQ(reloaded->Profiles().size(), 2u);
}

// Only the active profile takes a backend: another profile whose manifest records none (one from before the field)
// is not claimed, and its manifest is not written (release-compatibility RC017: no adoption kept).
TEST_F(BackendProfileTest, AnotherProfileWithoutABackendIsNotClaimed) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  const std::string staging_dir = registry->ActiveProfileDataDir();
  ASSERT_EQ(registry->ActiveProfileId(), "staging");
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  std::filesystem::remove(std::filesystem::path(staging_dir) / "manifest.json");
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(staging_dir))); // no backend

  EXPECT_FALSE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_TRUE(pbr::ReadProfileBackend(staging_dir).empty()) << "not written";
}

TEST_F(BackendProfileTest, AMatchingActiveProfileIsKept) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  // A fresh install has no data to protect: the empty default profile is adopted for the first backend.
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("custom-0123456789ab")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("custom-0123456789ab")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(registry->Profiles().size(), 1u);
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  EXPECT_EQ(registry->ActiveProfileId(), "prod") << "a recorded backend is not re-adopted";
}

TEST_F(BackendProfileTest, SessionProfileOverrideIsNotRedirected) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  registry->SetSessionProfileOverride("dev");
  ASSERT_TRUE(static_cast<bool>(registry->EnsureActiveProfile()));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("staging")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(std::filesystem::path(registry->ActiveProfileDataDir()).filename(), "dev");
}

TEST_F(BackendProfileTest, RecordedBackendIsNeverReplaced) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string dir = registry->ActiveProfileDataDir();
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(dir, "prod")));
  ASSERT_TRUE(static_cast<bool>(pbr::AdoptProfileBackend(dir, "staging")));
  EXPECT_EQ(pbr::ReadProfileBackend(dir), "prod");
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

TEST(BackendProfileKeyTest, ACustomProfileIsSeparatePerBuildKindWithTheSameHash) {
  const std::string url = "https://Custom.Example.test/api/relay/";
  const std::string store = pbr::CustomProfileKey(url, pbr::BuildKind::Store);
  const std::string tester = pbr::CustomProfileKey(url, pbr::BuildKind::Tester);
  EXPECT_EQ(store.rfind("custom-", 0), 0u);
  EXPECT_EQ(store.size(), 19u);
  EXPECT_EQ(tester.rfind("test-custom-", 0), 0u);
  EXPECT_EQ(tester.size(), 24u);
  EXPECT_EQ(tester.substr(12), store.substr(7)) << "the same hash";
  EXPECT_NE(tester, store);
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(store));
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(tester));
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Custom, url, kStore), store);
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Custom, url, kTesterAll), tester);
}

TEST(BackendProfileKeyTest, ATesterBuildNeverYieldsProdsKey) {
  for (const pbr::CompiledOrigins& compiled : {kTesterAll, kTesterDevOnly, kTesterNone}) {
    for (const pbr::BackendEnv env : kAllEnvs) {
      const std::string key = pbr::BackendProfileKeyFor(env, "https://custom.example.test/api/relay", compiled);
      EXPECT_NE(key, "prod");
      EXPECT_NE(key.rfind("custom-", 0), 0u) << key << ": a store build's custom profile";
      EXPECT_TRUE(pbr::ProfileBackendAllowed(key, pbr::BuildKind::Tester)) << key;
    }
  }
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kTesterAll), "staging");
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kTesterDevOnly), "dev");
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Staging, "", kTesterDevOnly), "dev");
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Dev, "", kTesterAll), "dev");
  // Neither staging nor dev compiled: a saved prod runs the custom backend (its URLs; none: no backend) under a tester
  // profile — never prod's.
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kTesterNone),
            pbr::CustomProfileKey("", pbr::BuildKind::Tester));
  // A store build, as before.
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kStore), "prod");
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Staging, "", kStore), "prod");
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
    EXPECT_EQ(pbr::BackendProfileKeyFor(env, "", kStagingPending), "dev") << pbr::BackendEnvName(env);
    EXPECT_EQ(pbr::BackendProfileKeyFor(env, "", kBothPending), pbr::CustomProfileKey("", pbr::BuildKind::Tester))
        << pbr::BackendEnvName(env);
  }
  constexpr pbr::CompiledOrigins kStorePending{.prod = "https://prod.example.test",
                                               .staging = "https://staging.example.test",
                                               .prod_relay_id = pbr::kPendingRelayId,
                                               .staging_relay_id = pbr::kPendingRelayId};
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Staging, "", kStorePending), "staging")
      << "a store build: as before";
  EXPECT_EQ(pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kStorePending), "prod");
}

TEST(BackendProfileKeyTest, ATesterBuildOpensNoStoreProfile) {
  for (const char* store_key : {"prod", "custom-0123456789ab"}) {
    EXPECT_FALSE(pbr::ProfileBackendAllowed(store_key, pbr::BuildKind::Tester)) << store_key;
    EXPECT_TRUE(pbr::ProfileBackendAllowed(store_key, pbr::BuildKind::Store)) << store_key;
  }
  for (const char* tester_key : {"", "staging", "dev", "qa", "test-custom-0123456789ab"}) {
    EXPECT_TRUE(pbr::ProfileBackendAllowed(tester_key, pbr::BuildKind::Tester)) << tester_key;
    EXPECT_TRUE(pbr::ProfileBackendAllowed(tester_key, pbr::BuildKind::Store)) << tester_key;
  }
}

// This build's running key against its kind.
TEST_F(BackendProfileTest, ThisBuildsKeysFollowItsKind) {
  pbr::AppConfig config;
  for (const pbr::BackendEnv env : kAllEnvs) {
    pbr::ResetActiveBackendForTest();
    pbr::LatchActiveBackend(env);
    EXPECT_TRUE(pbr::ProfileBackendAllowed(pbr::BackendProfileKey(config), pbr::ThisBuildKind()));
    config.backend.env = pbr::BackendEnvName(env);
    EXPECT_TRUE(pbr::ProfileBackendAllowed(pbr::SavedBackendKey(config), pbr::ThisBuildKind()));
  }
}
