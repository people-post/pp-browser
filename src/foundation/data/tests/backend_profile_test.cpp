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

} // namespace

TEST_F(BackendProfileTest, KeysAreStableAndFilesystemSafe) {
  pbr::AppConfig config;
  config.registration.base_url = "https://Custom.Example.test/api/relay/";
  pbr::LatchActiveBackend(pbr::BackendEnv::Custom);
  const std::string key = pbr::BackendProfileKey(config);
  EXPECT_EQ(key.rfind("custom-", 0), 0u);
  EXPECT_EQ(key.size(), 19u);
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(key));

  config.registration.base_url = "https://custom.example.test/api/relay";
  EXPECT_EQ(pbr::BackendProfileKey(config), key) << "case and a trailing slash do not name another backend";
  config.registration.base_url = "https://other.example.test/api/relay";
  EXPECT_NE(pbr::BackendProfileKey(config), key);
}

TEST_F(BackendProfileTest, ProdKeyAndEnvOriginKey) {
  pbr::AppConfig config;
  EXPECT_EQ(pbr::BackendProfileKey(config), "prod");
  SetOriginEnv("https://backend.example.test");
  const std::string key = pbr::BackendProfileKey(config);
  EXPECT_EQ(key.rfind("custom-", 0), 0u);
  SetOriginEnv(nullptr);
}

TEST_F(BackendProfileTest, SavedKeyDiffersFromRunningOneOnlyAfterASwitch) {
  pbr::AppConfig config;
  pbr::LatchActiveBackend(pbr::BackendEnv::Prod);
  EXPECT_EQ(pbr::SavedBackendKey(config), pbr::BackendProfileKey(config));

  config.backend.env = "custom";
  config.registration.base_url = "https://custom.example.test/api/relay";
  const std::string custom = pbr::SavedBackendKey(config);
  EXPECT_EQ(custom.rfind("custom-", 0), 0u);
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

TEST_F(BackendProfileTest, ExistingProfileIsAdoptedAsProdWithoutMovingData) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string dir = registry->ActiveProfileDataDir();
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(dir))); // pre-existing manifest: no backend
  EXPECT_TRUE(pbr::ReadProfileBackend(dir).empty());

  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(registry->ActiveProfileDataDir(), dir);
  EXPECT_EQ(pbr::ReadProfileBackend(dir), "prod");
}

TEST_F(BackendProfileTest, QaGetsItsOwnProfileAndPreExistingOneBecomesProd) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string prod_dir = registry->ActiveProfileDataDir();

  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("qa")));
  EXPECT_EQ(registry->ActiveProfileId(), "qa");
  EXPECT_EQ(pbr::ReadProfileBackend(prod_dir), "prod");
  EXPECT_EQ(pbr::ReadProfileBackend(registry->ActiveProfileDataDir()), "qa");

  // Switching back finds the first profile instead of creating another; the choice persists.
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  auto reloaded = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->ActiveProfileId(), "default");
  EXPECT_EQ(reloaded->Profiles().size(), 2u);

  ASSERT_TRUE(static_cast<bool>(reloaded->SelectProfileForBackend("qa")));
  EXPECT_EQ(reloaded->ActiveProfileId(), "qa");
  EXPECT_EQ(reloaded->Profiles().size(), 2u);
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
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("qa")));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(std::filesystem::path(registry->ActiveProfileDataDir()).filename(), "dev");
}

TEST_F(BackendProfileTest, RecordedBackendIsNeverReplaced) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  const std::string dir = registry->ActiveProfileDataDir();
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(dir, "prod")));
  ASSERT_TRUE(static_cast<bool>(pbr::AdoptProfileBackend(dir, "qa")));
  EXPECT_EQ(pbr::ReadProfileBackend(dir), "prod");
}
