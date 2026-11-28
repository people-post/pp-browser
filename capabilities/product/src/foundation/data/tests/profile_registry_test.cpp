// Profiles under opaque ids, one home profile per backend, a manifest that records the network (profile-model PM001,
// PM003, PM008, PM009, PM012; P1, #752).
#include "foundation/data/ProfileRegistry.h"
#include "foundation/data/ProfileManifest.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kMain = pbr::profile_network::kMain;
constexpr std::string_view kStaging = pbr::profile_network::kStaging;
constexpr std::string_view kDev = pbr::profile_network::kDev;
constexpr std::string_view kNode = pbr::profile_network::kNode;

std::string UniquePath(const char* prefix) {
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return (fs::temp_directory_path() / (std::string(prefix) + std::to_string(now))).string();
}

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

class ProfileRegistryTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = UniquePath("pp_browser_profile_registry_");
    fs::remove_all(data_dir_);
  }
  void TearDown() override { fs::remove_all(data_dir_); }

  /** One start of a build running `backend` on `network`: Load, then the selection Bootstrap does. */
  std::string Start(const std::string& backend, std::string_view network) {
    auto registry = pbr::ProfileRegistry::Load(data_dir_);
    EXPECT_TRUE(static_cast<bool>(registry));
    auto selected = registry->SelectProfileForBackend(backend, network);
    EXPECT_TRUE(static_cast<bool>(selected)) << (selected ? "" : selected.error().message);
    return registry->ActiveProfileId();
  }

  /** Reset my data as Application does it: the old folder goes first, then a new profile takes its place. */
  std::string Reset(const std::string& backend, std::string_view network) {
    auto registry = pbr::ProfileRegistry::Load(data_dir_);
    EXPECT_TRUE(static_cast<bool>(registry));
    EXPECT_TRUE(static_cast<bool>(registry->SelectProfileForBackend(backend, network)));
    fs::remove_all(registry->ActiveProfileDataDir());
    auto replaced = registry->ReplaceActiveProfile();
    EXPECT_TRUE(static_cast<bool>(replaced)) << (replaced ? "" : replaced.error().message);
    return registry->ActiveProfileId();
  }

  fs::path ProfileDir(const std::string& id) const { return fs::path(data_dir_) / "profiles" / id; }
  bool HasBackend(const std::string& id, const std::string& backend) const {
    return fs::is_directory(ProfileDir(id) / "backends" / backend);
  }

  std::string data_dir_;
};

} // namespace

TEST_F(ProfileRegistryTest, AFirstStartMakesAnOpaqueProfileMarkedWithItsNetworkAndBackend) {
  const std::string id = Start("prod", kMain);
  EXPECT_EQ(id.size(), 16u);
  EXPECT_NE(id, "prod");
  EXPECT_NE(id, "default");
  const pbr::ProfileManifestRead manifest = pbr::ReadProfileManifest(ProfileDir(id).string());
  EXPECT_EQ(manifest.state, pbr::ProfileManifestState::kCurrent);
  EXPECT_EQ(manifest.network, kMain);
  EXPECT_TRUE(HasBackend(id, "prod"));

  const std::string registry_text = ReadText(fs::path(data_dir_) / "profiles.json");
  EXPECT_NE(registry_text.find("\"schema_version\": 2"), std::string::npos) << registry_text;
  EXPECT_EQ(registry_text.find("active_profile_id"), std::string::npos) << "no device-wide active profile";
  EXPECT_EQ(Start("prod", kMain), id) << "the next start opens the same profile";
}

TEST_F(ProfileRegistryTest, EachBackendHasItsOwnHomeProfileAndSwitchingBackFindsIt) {
  const std::string prod = Start("prod", kMain);
  const std::string staging = Start("staging", kStaging);
  const std::string dev = Start("dev", kDev);
  EXPECT_NE(prod, staging);
  EXPECT_NE(staging, dev);
  EXPECT_EQ(Start("prod", kMain), prod);
  EXPECT_EQ(Start("staging", kStaging), staging);

  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->Profiles().size(), 3u);
  EXPECT_EQ(registry->BackendPointers().at("prod"), prod);
  EXPECT_EQ(registry->BackendPointers().at("staging"), staging);
}

// A store build and a tester build share one data root on a phone: each keeps its own pointer, neither moves the other's.
TEST_F(ProfileRegistryTest, AStoreBuildAndATesterBuildNeverMoveEachOthersProfile) {
  const std::string store = Start("prod", kMain);
  const std::string tester = Start("staging", kStaging);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(Start("prod", kMain), store);
    EXPECT_EQ(Start("staging", kStaging), tester);
  }
}

// #733 scenario A: a store profile reset, then the tester build, then the store build again. Both start, each on its
// own profile.
TEST_F(ProfileRegistryTest, Issue733AStoreResetThenTesterThenStore) {
  Start("staging", kStaging); // the tester build ran first
  Start("prod", kMain);
  const std::string store_after_reset = Reset("prod", kMain);
  const std::string tester = Start("staging", kStaging);
  EXPECT_NE(tester, store_after_reset) << "the reset profile is not the tester build's to take";
  EXPECT_EQ(Start("prod", kMain), store_after_reset);
}

// #733 scenario B: the store build's first profile reset, then tester, then store.
TEST_F(ProfileRegistryTest, Issue733BFirstStoreProfileResetThenTesterThenStore) {
  Start("prod", kMain);
  const std::string store_after_reset = Reset("prod", kMain);
  const std::string tester = Start("staging", kStaging);
  EXPECT_NE(tester, store_after_reset);
  EXPECT_EQ(Start("prod", kMain), store_after_reset) << "not a new empty profile";
}

// #733 scenario C: the other direction, a tester profile reset, then store, then tester.
TEST_F(ProfileRegistryTest, Issue733CTesterResetThenStoreThenTester) {
  Start("prod", kMain);
  Start("staging", kStaging);
  const std::string tester_after_reset = Reset("staging", kStaging);
  const std::string store = Start("prod", kMain);
  EXPECT_NE(store, tester_after_reset);
  EXPECT_EQ(Start("staging", kStaging), tester_after_reset);
}

// #738: reset, then a backend switch saved before the restart, then back.
TEST_F(ProfileRegistryTest, Issue738ResetThenSwitchBeforeRestartThenBack) {
  Start("staging", kStaging);
  const std::string staging_after_reset = Reset("staging", kStaging);
  const std::string dev = Start("dev", kDev);
  EXPECT_NE(dev, staging_after_reset);
  EXPECT_EQ(Start("staging", kStaging), staging_after_reset);
  EXPECT_EQ(Start("dev", kDev), dev);
}

TEST_F(ProfileRegistryTest, AResetKeepsTheLabelAndRepointsOnlyItsBackend) {
  const std::string prod = Start("prod", kMain);
  const std::string staging = Start("staging", kStaging);
  const std::string reset = Reset("prod", kMain);
  EXPECT_NE(reset, prod);
  EXPECT_FALSE(fs::exists(ProfileDir(prod)));
  EXPECT_TRUE(HasBackend(reset, "prod"));
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->BackendPointers().at("prod"), reset);
  EXPECT_EQ(registry->BackendPointers().at("staging"), staging);
  EXPECT_EQ(registry->Profiles().size(), 2u);
}

// Stopped between removing the old folder and making the new one: the pointer names a folder that is gone, and the
// next start makes the new profile.
TEST_F(ProfileRegistryTest, AResetStoppedHalfWayMakesTheNewProfileAtTheNextStart) {
  const std::string prod = Start("prod", kMain);
  fs::remove_all(ProfileDir(prod));
  const std::string next = Start("prod", kMain);
  EXPECT_NE(next, prod);
  EXPECT_TRUE(HasBackend(next, "prod"));
}

TEST_F(ProfileRegistryTest, ALostPointerFindsTheHomeProfileByItsBackendFolder) {
  const std::string prod = Start("prod", kMain);
  const std::string staging = Start("staging", kStaging);
  fs::remove(fs::path(data_dir_) / "profiles.json");
  EXPECT_EQ(Start("staging", kStaging), staging);
  EXPECT_EQ(Start("prod", kMain), prod);
}

TEST_F(ProfileRegistryTest, AProfileOnAnotherNetworkIsNeverTaken) {
  const std::string staging = Start("staging", kStaging);
  fs::create_directories(ProfileDir(staging) / "backends" / "prod"); // as if planted
  fs::remove(fs::path(data_dir_) / "profiles.json");
  const std::string prod = Start("prod", kMain);
  EXPECT_NE(prod, staging);
}

TEST_F(ProfileRegistryTest, AFolderWithoutAReadableOrCurrentManifestIsNeverClaimed) {
  fs::create_directories(ProfileDir("nomanifest") / "backends" / "prod");
  WriteText(ProfileDir("garbled") / "manifest.json", "{ not json");
  fs::create_directories(ProfileDir("garbled") / "backends" / "prod");
  WriteText(ProfileDir("newer") / "manifest.json", R"({"schema_version": 3, "network": "main"})");
  fs::create_directories(ProfileDir("newer") / "backends" / "prod");
  WriteText(ProfileDir("nonetwork") / "manifest.json", R"({"schema_version": 2})");
  fs::create_directories(ProfileDir("nonetwork") / "backends" / "prod");

  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->SkippedProfiles(), (std::vector<std::string>{"garbled", "newer", "nomanifest", "nonetwork"}));
  const std::string id = Start("prod", kMain);
  for (const char* kept : {"nomanifest", "garbled", "newer", "nonetwork"}) {
    EXPECT_NE(id, kept);
    EXPECT_TRUE(fs::exists(ProfileDir(kept))) << kept << " is left alone";
  }
  EXPECT_EQ(ReadText(ProfileDir("newer") / "manifest.json"), R"({"schema_version": 3, "network": "main"})");
}

// A downgrade must not point a backend at an empty profile: the pointer at a newer profile stays, and the start fails.
TEST_F(ProfileRegistryTest, APointerAtANewerProfileIsAnErrorAndIsNotMoved) {
  const std::string prod = Start("prod", kMain);
  WriteText(ProfileDir(prod) / "manifest.json", R"({"schema_version": 3, "network": "main"})");
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_FALSE(static_cast<bool>(registry->SelectProfileForBackend("prod", kMain)));
  auto reloaded = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(reloaded));
  EXPECT_EQ(reloaded->BackendPointers().at("prod"), prod);
}

TEST_F(ProfileRegistryTest, AHalfMadeProfileIsRemovedAndNeverOpened) {
  WriteText(fs::path(data_dir_) / "profiles" / ".new-0123456789abcdef" / "manifest.json",
            R"({"schema_version": 2, "network": "main"})");
  fs::create_directories(fs::path(data_dir_) / "profiles" / ".new-0123456789abcdef" / "backends" / "prod");
  const std::string id = Start("prod", kMain);
  EXPECT_NE(id, ".new-0123456789abcdef");
  EXPECT_FALSE(fs::exists(fs::path(data_dir_) / "profiles" / ".new-0123456789abcdef"));
}

TEST_F(ProfileRegistryTest, ALabelResolvesAmongTheRunningBackendsProfilesOnly) {
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileByLabel("work", "staging", kStaging)));
  const std::string staging_work = registry->ActiveProfileId();
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileByLabel("work", "prod", kMain)));
  const std::string prod_work = registry->ActiveProfileId();
  EXPECT_NE(prod_work, staging_work) << "another backend's profile of that label is not opened";
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileByLabel("work", "staging", kStaging)));
  EXPECT_EQ(registry->ActiveProfileId(), staging_work);
  EXPECT_TRUE(registry->BackendPointers().empty()) << "a label never moves a backend's pointer";

  EXPECT_NE(Start("prod", kMain), prod_work) << "the backend's own home profile is not the labelled one";
  for (const char* bad : {"", "a\nb"}) {
    EXPECT_FALSE(static_cast<bool>(registry->SelectProfileByLabel(bad, "prod", kMain))) << bad;
  }
}

// PB-041: a profile id names a directory under profiles/ and nothing else.
TEST_F(ProfileRegistryTest, NothingReadFromProfilesJsonReachesOutsideTheProfilesDirectory) {
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId("default"));
  EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId("node-2.test_B"));
  for (const char* bad : {"", "..", ".hidden", "../x", "a/b", "a\\b", "/etc", "C:x", "a b", "a\nb"}) {
    EXPECT_FALSE(pbr::ProfileRegistry::IsValidProfileId(bad)) << bad;
  }
  EXPECT_FALSE(pbr::ProfileRegistry::IsValidProfileId(std::string(65, 'a')));

  WriteText(fs::path(data_dir_) / "profiles.json",
            R"({"schema_version": 2, "active": {"prod": "../../x", "../y": "abc"},
                "profiles": [{"id": "../../x", "label": "x"}, {"id": "work", "label": "w"}]})");
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_TRUE(registry->BackendPointers().empty());
  for (const pbr::ProfileEntry& entry : registry->Profiles()) {
    EXPECT_TRUE(pbr::ProfileRegistry::IsValidProfileId(entry.id)) << entry.id;
  }
  EXPECT_FALSE(static_cast<bool>(registry->SelectProfileForBackend("../escaped", kMain)));
  EXPECT_FALSE(fs::exists(fs::path(data_dir_) / "escaped"));
}

TEST_F(ProfileRegistryTest, ANewerProfilesFileIsRefused) {
  WriteText(fs::path(data_dir_) / "profiles.json", R"({"schema_version": 3})");
  EXPECT_FALSE(static_cast<bool>(pbr::ProfileRegistry::Load(data_dir_)));
}

// The per-backend layout (profiles.json and manifests of schema 1): listed as the old format, read as nothing else.
TEST_F(ProfileRegistryTest, TheOldPerBackendLayoutIsListedAsTheOldFormat) {
  WriteText(fs::path(data_dir_) / "profiles.json",
            R"({"schema_version": 1, "active_profile_id": "prod",
                "profiles": [{"id": "default", "label": "user"}, {"id": "prod", "label": "prod"}]})");
  WriteText(ProfileDir("default") / "manifest.json", R"({"schema_version": 1, "backend": "staging"})");
  WriteText(ProfileDir("prod") / "manifest.json", R"({"schema_version": 1})");
  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  EXPECT_EQ(registry->OldFormatProfiles(), (std::vector<std::string>{"default", "prod"}));
  EXPECT_TRUE(registry->Profiles().empty());
  EXPECT_TRUE(registry->BackendPointers().empty());

  // As Bootstrap does: each removed, then forgotten; the backend then gets a profile of the new layout.
  for (const std::string& id : registry->OldFormatProfiles()) {
    fs::remove_all(ProfileDir(id));
    ASSERT_TRUE(static_cast<bool>(registry->ForgetProfile(id)));
  }
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("prod", kMain)));
  EXPECT_NE(registry->ActiveProfileId(), "prod");
  EXPECT_TRUE(registry->OldFormatProfiles().empty());
}

// pp-node (PM007): a deployed node's profile from before keeps its folder, keys and id, now on network `node`.
TEST_F(ProfileRegistryTest, APpNodeProfileOfTheOldLayoutIsConvertedInPlace) {
  WriteText(
      fs::path(data_dir_) / "profiles.json",
      R"({"schema_version": 1, "active_profile_id": "default", "profiles": [{"id": "default", "label": "user"}]})");
  WriteText(ProfileDir("default") / "manifest.json", R"({"schema_version": 1})");
  WriteText(ProfileDir("default") / "identity.enc", "sealed");
  WriteText(ProfileDir("edge-2") / "manifest.json", R"({"schema_version": 1})");

  auto registry = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(registry));
  ASSERT_TRUE(static_cast<bool>(registry->ConvertOldFormatProfilesToNode()));
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileForBackend("node", kNode)));
  EXPECT_EQ(registry->ActiveProfileId(), "default");
  EXPECT_EQ(ReadText(ProfileDir("default") / "identity.enc"), "sealed");
  EXPECT_EQ(pbr::ReadProfileManifest(ProfileDir("default").string()).network, kNode);

  // `--profile edge-2` as before: the old id is the label of a converted profile that is not the home one.
  ASSERT_TRUE(static_cast<bool>(registry->SelectProfileByLabel("edge-2", "node", kNode)));
  EXPECT_EQ(registry->ActiveProfileId(), "edge-2");

  // Once: the next start has nothing to convert and opens the same profile.
  auto again = pbr::ProfileRegistry::Load(data_dir_);
  ASSERT_TRUE(static_cast<bool>(again));
  EXPECT_TRUE(again->OldFormatProfiles().empty());
  ASSERT_TRUE(static_cast<bool>(again->ConvertOldFormatProfilesToNode()));
  ASSERT_TRUE(static_cast<bool>(again->SelectProfileForBackend("node", kNode)));
  EXPECT_EQ(again->ActiveProfileId(), "default");

  // profiles.json lost on the volume: the home profile is found again by its backend folder, not a new identity.
  fs::remove(fs::path(data_dir_) / "profiles.json");
  EXPECT_EQ(Start("node", kNode), "default");
}

TEST(ProfileManifestTest, ANewManifestIsNeverWrittenOverAnother) {
  const std::string dir = UniquePath("pp_browser_profile_manifest_");
  ASSERT_TRUE(static_cast<bool>(pbr::WriteNewProfileManifest(dir, kStaging)));
  EXPECT_FALSE(static_cast<bool>(pbr::WriteNewProfileManifest(dir, kMain)));
  EXPECT_EQ(pbr::ReadProfileManifest(dir).network, kStaging);
  EXPECT_FALSE(static_cast<bool>(pbr::WriteNewProfileManifest(dir + "-x", "nowhere")));
  EXPECT_FALSE(static_cast<bool>(pbr::ConvertOldProfileManifestToNode(dir))) << "only the old format converts";
  fs::remove_all(dir);
}
