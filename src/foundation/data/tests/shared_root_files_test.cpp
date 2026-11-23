// The data root's shared files — config.json, profiles.json, machine.json — are read by every build on the device: a
// tester build and the store build share one data container on a phone (release-compatibility RC022). Their versions
// change only in a release; a tester build may only add optional keys, which older builds drop or ignore
// (docs/contracts/DATA_LAYOUT.md § Shared root files).
#include "foundation/data/BackendProfile.h"
#include "foundation/data/Config.h"
#include "foundation/data/ProfileManifest.h"
#include "foundation/data/ProfileRegistry.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/platform/DeploymentProfile.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (std::filesystem::temp_directory_path() / (std::string(prefix) + std::to_string(now))).string();
}

std::string ReadText(const std::string& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteText(const std::string& path, const std::string& text) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream out(path);
  out << text;
}

void UnsetOriginEnv() {
#ifdef _WIN32
  _putenv_s("PP_BROWSER_ORIGIN", "");
#else
  unsetenv("PP_BROWSER_ORIGIN");
#endif
}

class SharedRootFilesTest : public ::testing::Test {
protected:
  void SetUp() override {
    pbr::ResetActiveBackendForTest();
    UnsetOriginEnv();
    data_dir_ = UniquePath("pp_browser_shared_root_");
    std::filesystem::remove_all(data_dir_);
  }
  void TearDown() override {
    pbr::ResetActiveBackendForTest();
    std::filesystem::remove_all(data_dir_);
  }
  std::string ProfileDir(const std::string& id) const {
    return (std::filesystem::path(data_dir_) / "profiles" / id).string();
  }
  std::string data_dir_;
};

} // namespace

// A deliberate pin. Bumping one of these is a release decision, not a tester build's: every build on the device reads
// these files, and an older build refuses a newer version (it would not start). Before changing a number here, read
// DATA_LAYOUT.md § Shared root files (release-compatibility RC022) — the bump ships in a release, never first in a
// tester build.
TEST(SharedRootFileVersionsTest, EachRootFilesVersionIsPinned) {
  EXPECT_EQ(pbr::Config::kConfigVersion, 1) << "config.json config_version";
  EXPECT_EQ(pbr::ProfileRegistry::kSchemaVersion, 1) << "profiles.json schema_version";
  EXPECT_EQ(pbr::MachinePreferences::kSchemaVersion, 1) << "machine.json schema_version";
}

// A tester build made its profiles (staging, dev, test-custom-…) in the shared profiles.json and left one active; the
// store build reads that file, goes back to its own prod profile, keeps every entry, and changes no tester profile.
TEST_F(SharedRootFilesTest, AStoreBuildReadsAProfilesFileATesterBuildWrote) {
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(ProfileDir("default"), "prod")));
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(ProfileDir("staging"), "staging")));
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(ProfileDir("dev"), "dev")));
  const std::string tester_custom = pbr::CustomProfileKey("https://custom.example.test", pbr::BuildKind::Tester);
  ASSERT_TRUE(static_cast<bool>(pbr::EnsureProfileManifest(ProfileDir(tester_custom), tester_custom)));
  WriteText((std::filesystem::path(data_dir_) / "profiles.json").string(),
            R"({"schema_version": 1, "active_profile_id": ")" + tester_custom + R"(",
                "profiles": [{"id": "default", "label": "user"}, {"id": "staging", "label": "staging"},
                             {"id": "dev", "label": "dev"},
                             {"id": ")" +
                tester_custom + R"(", "label": ")" + tester_custom + R"("}]})");

  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->Profiles().size(), 4u);
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  EXPECT_EQ(registry->ActiveProfileId(), "default") << "the store build's own profile";
  EXPECT_TRUE(
      pbr::ProfileBackendAllowed(pbr::ReadProfileBackend(registry->ActiveProfileDataDir()), pbr::BuildKind::Store));

  auto reloaded = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->ActiveProfileId(), "default");
  EXPECT_EQ(reloaded->Profiles().size(), 4u) << "the tester build's entries are kept";
  EXPECT_EQ(pbr::ReadProfileBackend(ProfileDir("staging")), "staging");
  EXPECT_EQ(pbr::ReadProfileBackend(ProfileDir(tester_custom)), tester_custom);

  // A store build's custom backend at the same URL is a profile of its own, not the tester build's.
  ASSERT_TRUE(static_cast<bool>(
      reloaded->SelectProfileForBackend(pbr::CustomProfileKey("https://custom.example.test", pbr::BuildKind::Store))));
  EXPECT_NE(reloaded->ActiveProfileId(), tester_custom);
  EXPECT_EQ(reloaded->Profiles().size(), 5u);
}

// The other way round: a tester build on a device with the store build's prod profile makes its own and leaves prod's
// manifest as it was.
TEST_F(SharedRootFilesTest, ATesterBuildMakesItsOwnProfileBesideTheStoreBuilds) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod")));
  const std::string prod_dir = registry->ActiveProfileDataDir();
  const std::string prod_manifest = ReadText((std::filesystem::path(prod_dir) / "manifest.json").string());

  constexpr pbr::CompiledOrigins kTester{.staging = "https://staging.example.test", .kind = pbr::BuildKind::Tester};
  const std::string key = pbr::BackendProfileKeyFor(pbr::BackendEnv::Prod, "", kTester); // a saved prod
  ASSERT_EQ(key, "staging");
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend(key)));
  EXPECT_EQ(registry->ActiveProfileId(), "staging");
  EXPECT_TRUE(
      pbr::ProfileBackendAllowed(pbr::ReadProfileBackend(registry->ActiveProfileDataDir()), pbr::BuildKind::Tester));
  EXPECT_FALSE(pbr::ProfileBackendAllowed(pbr::ReadProfileBackend(prod_dir), pbr::BuildKind::Tester));
  EXPECT_EQ(ReadText((std::filesystem::path(prod_dir) / "manifest.json").string()), prod_manifest) << "untouched";
}

// The saved choice survives a build that cannot run it: a tester build reading `prod` (or a store build reading a
// staging / dev it has no origin for) runs another, and a save writes the file's choice back.
TEST_F(SharedRootFilesTest, ABuildThatCannotRunTheSavedChoiceDoesNotRewriteIt) {
  const pbr::CompiledOrigins compiled = pbr::BuildCompiledOrigins();
  const char* unavailable = nullptr;
  for (const pbr::BackendEnv env : {pbr::BackendEnv::Prod, pbr::BackendEnv::Staging, pbr::BackendEnv::Dev}) {
    if (!pbr::BackendAvailableIn(env, compiled)) {
      unavailable = pbr::BackendEnvName(env);
      break;
    }
  }
  if (unavailable == nullptr) {
    GTEST_SKIP() << "this build runs every environment (a store build with staging and dev compiled)";
  }
  if (pbr::IsTesterBuild()) {
    EXPECT_STREQ(unavailable, "prod");
  }
  const std::string path = (std::filesystem::path(data_dir_) / "config.json").string();
  WriteText(path, std::string(R"({"config_version": 1, "backend": {"env": ")") + unavailable + R"("}})");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, pbr::BackendEnvName(pbr::ActiveBackend())) << "runs what this build can";
  EXPECT_NE(loaded->backend.env, unavailable);
  EXPECT_EQ(loaded->backend.unavailable_env, unavailable);

  ASSERT_TRUE(static_cast<bool>(pbr::Config::SaveToFile(path, *loaded)));
  const std::string text = ReadText(path);
  EXPECT_NE(text.find(std::string("\"env\": \"") + unavailable + "\""), std::string::npos) << text;

  pbr::ResetActiveBackendForTest();
  auto again = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(again));
  EXPECT_EQ(again->backend.unavailable_env, unavailable) << "still the file's choice";
}

TEST_F(SharedRootFilesTest, AChoiceThisBuildRunsIsSavedAsIs) {
  const std::string path = (std::filesystem::path(data_dir_) / "config.json").string();
  WriteText(path, R"({"config_version": 1, "backend": {"env": "custom"}})");
  auto loaded = pbr::Config::LoadFromFile(path);
  ASSERT_TRUE(static_cast<bool>(loaded));
  EXPECT_EQ(loaded->backend.env, "custom");
  EXPECT_TRUE(loaded->backend.unavailable_env.empty());
  ASSERT_TRUE(static_cast<bool>(pbr::Config::SaveToFile(path, *loaded)));
  EXPECT_NE(ReadText(path).find("\"env\": \"custom\""), std::string::npos);
}
