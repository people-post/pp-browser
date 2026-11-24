// "Later" on the Chats list's new-version card is three optional fields of machine.json: absent = never tapped, an
// old file reads unchanged, and an older build (known keys only) reads a file that carries them as before.
#include "feature/settings/UpdatePrefs.h"

#include "common/ValueJson.h"
#include "foundation/data/ConfigJson.h"
#include "foundation/data/UserPreferences.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace pbr {
namespace {

namespace fs = std::filesystem;

class UpdatePrefsTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           (std::string("pp-update-prefs-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override { fs::remove_all(dir_); }
  fs::path dir_;
};

TEST_F(UpdatePrefsTest, AFreshDeviceHasNeverTappedLater) {
  const UpdateCardState state = UpdatePrefs::Read(dir_.string());
  EXPECT_EQ(state.version, "");
  EXPECT_EQ(state.dismissals, 0);
  EXPECT_EQ(state.dismissed_ms, 0);
}

TEST_F(UpdatePrefsTest, RoundTripsAndKeepsTheOtherSettings) {
  auto machine = UserPreferences::LoadMachine(dir_.string());
  ASSERT_TRUE(machine);
  machine->collect_run_logs = true;
  ASSERT_TRUE(UserPreferences::SaveMachine(dir_.string(), *machine));
  ASSERT_TRUE(UpdatePrefs::Write(dir_.string(), UpdateCardState{"1.2.0", 1, 1700000000000}));
  const UpdateCardState state = UpdatePrefs::Read(dir_.string());
  EXPECT_EQ(state.version, "1.2.0");
  EXPECT_EQ(state.dismissals, 1);
  EXPECT_EQ(state.dismissed_ms, 1700000000000);
  auto back = UserPreferences::LoadMachine(dir_.string());
  ASSERT_TRUE(back);
  EXPECT_TRUE(back->collect_run_logs);
  EXPECT_EQ(back->schema_version, MachinePreferences::kSchemaVersion) << "no schema bump";
}

TEST_F(UpdatePrefsTest, AnOldFileWithoutTheKeysReadsAsBefore) {
  MachinePreferences prefs;
  MachinePrefsFromObject(
      *TryParseObject(R"({"schema_version":1,"active_profile_id":"default","collect_run_logs":true})"), prefs);
  EXPECT_TRUE(prefs.collect_run_logs);
  EXPECT_EQ(prefs.update_card_version, "");
  EXPECT_EQ(prefs.update_card_dismissals, 0);
  EXPECT_EQ(prefs.update_card_dismissed_ms, 0);
}

TEST_F(UpdatePrefsTest, NothingNewIsWrittenUntilLaterWasTapped) {
  const auto object = MachinePrefsToObject(MachinePreferences{});
  EXPECT_FALSE(object.contains("update_card_version"));
  EXPECT_FALSE(object.contains("update_card_dismissals"));
  EXPECT_FALSE(object.contains("update_card_dismissed_ms"));
}

TEST_F(UpdatePrefsTest, AFileWithTheKeysAndAnUnknownKeyStillReadsEveryOtherFieldAsWritten) {
  // Runs the current reader over a file that also carries a key nobody knows: the other fields come out as written.
  // (A real older build is not run here; it reads known keys only and so drops these three at its next save.)
  MachinePreferences prefs;
  MachinePrefsFromObject(*TryParseObject(R"({"schema_version":1,"active_profile_id":"qa","collect_crash_logs":true,
      "update_card_version":"1.2.0","update_card_dismissals":1,"update_card_dismissed_ms":5,"a_future_key":{"x":1}})"),
                         prefs);
  EXPECT_TRUE(prefs.collect_crash_logs);
  EXPECT_EQ(prefs.schema_version, 1);
  EXPECT_EQ(prefs.update_card_dismissals, 1);
}

TEST_F(UpdatePrefsTest, OddValuesAreClamped) {
  MachinePreferences prefs;
  MachinePrefsFromObject(*TryParseObject(R"({"update_card_dismissals":9,"update_card_dismissed_ms":-5})"), prefs);
  EXPECT_EQ(prefs.update_card_dismissals, 2);
  EXPECT_EQ(prefs.update_card_dismissed_ms, 0);
}

} // namespace
} // namespace pbr
