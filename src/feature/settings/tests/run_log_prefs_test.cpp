// The device-wide "Collect run logs" switch and when old logs are deleted (DEV/PLAN-send-run-logs.md): only after the
// switch was read, never when it cannot be, the same for every profile and backend, and down to two launches when
// the switch is off, turned off or the feature's gate is closed.
#include "feature/settings/RunLogPrefs.h"

#include "foundation/data/ConfigJson.h"
#include "foundation/data/UserPreferences.h"
#include "foundation/diagnostics/LogFile.h"
#include "common/ValueJson.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace pbr {
namespace {

namespace fs = std::filesystem;

void WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::trunc);
  out << text;
}

std::string ReadAll(const fs::path& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

class RunLogPrefsTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = fs::temp_directory_path() /
                (std::string("pp-run-log-prefs-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    fs::remove_all(data_dir_, ec);
    fs::create_directories(data_dir_ / "logs");
    log_path_ = LogFile::DefaultPath(data_dir_.string());
  }
  void TearDown() override {
    std::error_code ec;
    fs::remove_all(data_dir_, ec);
  }

  /** The running launch's file and `older` earlier launches, launch i about i days old (within 7 days). */
  void Launches(const int older) {
    WriteText(log_path_, "current");
    for (int i = 1; i <= older; ++i) {
      const std::string name = LogFile::RotatedPath(log_path_, static_cast<std::size_t>(i));
      WriteText(name, "launch " + std::to_string(i));
      fs::last_write_time(name, fs::file_time_type::clock::now() - std::chrono::hours(20) * i);
    }
  }
  int Files() const { return static_cast<int>(LogFile::FamilyFiles(log_path_).size()); }
  /** A restart: this launch's file becomes the previous one, a new one is opened. */
  void Restart() {
    LogFile::Rotate(log_path_);
    WriteText(log_path_, "current");
  }
  void SaveMachine(const bool collect, const std::string& active_profile = "default") {
    MachinePreferences prefs;
    prefs.collect_run_logs = collect;
    prefs.active_profile_id = active_profile;
    ASSERT_TRUE(UserPreferences::SaveMachine(data_dir_.string(), prefs));
  }
  std::string Dir() const { return data_dir_.string(); }

  fs::path data_dir_;
  std::string log_path_;
};

TEST_F(RunLogPrefsTest, OffByDefaultOnAFreshDevice) {
  EXPECT_EQ(RunLogPrefs::ReadCollect(Dir(), /*allowed=*/true), std::optional<bool>(false));
  EXPECT_FALSE(MachinePreferences{}.collect_run_logs);
}

TEST_F(RunLogPrefsTest, TheSwitchIsAnOptionalFieldOfMachineJson) {
  MachinePreferences prefs;
  MachinePrefsFromObject(*TryParseObject(R"({"schema_version":1,"active_profile_id":"default"})"), prefs);
  EXPECT_FALSE(prefs.collect_run_logs) << "absent = off";
  prefs.collect_run_logs = true;
  MachinePreferences back;
  MachinePrefsFromObject(MachinePrefsToObject(prefs), back);
  EXPECT_TRUE(back.collect_run_logs);
  EXPECT_EQ(back.schema_version, MachinePreferences::kSchemaVersion);
}

TEST_F(RunLogPrefsTest, OnKeepsEveryLaunchOfTheLastSevenDaysAcrossARestart) {
  SaveMachine(true);
  Launches(5);
  Restart();
  ASSERT_TRUE(RunLogPrefs::PruneAtStartup(Dir(), log_path_, /*allowed=*/true));
  EXPECT_EQ(Files(), 7) << "the current file and six earlier launches, all within 7 days";
}

// The log directory is the device's; a backend switch (another profile) does not turn the switch off.
TEST_F(RunLogPrefsTest, SwitchingBackendAndRestartingKeepsTheSevenDays) {
  ASSERT_TRUE(RunLogPrefs::SetCollect(Dir(), log_path_, true));
  Launches(4);
  auto machine = UserPreferences::LoadMachine(Dir());
  ASSERT_TRUE(machine);
  machine->active_profile_id = "qa"; // as the backend switch's restart leaves it
  ASSERT_TRUE(UserPreferences::SaveMachine(Dir(), *machine));
  fs::create_directories(data_dir_ / "profiles" / "qa");
  WriteText(data_dir_ / "profiles" / "qa" / "preferences.json", R"({"schema_version":16})");
  Restart();
  ASSERT_TRUE(RunLogPrefs::PruneAtStartup(Dir(), log_path_, true));
  EXPECT_EQ(Files(), 6);
}

TEST_F(RunLogPrefsTest, AnUnreadableSwitchDeletesNothingAndPackagesAsOff) {
  Launches(5);
  WriteText(data_dir_ / "machine.json", "{ not json");
  EXPECT_EQ(RunLogPrefs::ReadCollect(Dir(), true), std::nullopt);
  EXPECT_FALSE(RunLogPrefs::PruneAtStartup(Dir(), log_path_, true));
  EXPECT_EQ(Files(), 6) << "nothing deleted";
  EXPECT_EQ(RunLogPrefs::Retention(Dir(), true).max_age, std::chrono::hours(0)) << "unknown = off for packaging";
}

TEST_F(RunLogPrefsTest, OffLeavesThisLaunchAndThePreviousOne) {
  SaveMachine(false);
  Launches(5);
  ASSERT_TRUE(RunLogPrefs::PruneAtStartup(Dir(), log_path_, true));
  EXPECT_EQ(Files(), 2);
  EXPECT_EQ(ReadAll(LogFile::RotatedPath(log_path_, 1)), "launch 1");
}

TEST_F(RunLogPrefsTest, AClosedGateIgnoresAStoredOnAndKeepsTwoLaunches) {
  SaveMachine(true);
  Launches(5);
  EXPECT_EQ(RunLogPrefs::ReadCollect(Dir(), /*allowed=*/false), std::optional<bool>(false));
  ASSERT_TRUE(RunLogPrefs::PruneAtStartup(Dir(), log_path_, /*allowed=*/false));
  EXPECT_EQ(Files(), 2);
  auto machine = UserPreferences::LoadMachine(Dir());
  ASSERT_TRUE(machine);
  EXPECT_TRUE(machine->collect_run_logs) << "the stored value is ignored, not cleared";
}

TEST_F(RunLogPrefsTest, TurningItOffPrunesAtOnceAndOnDeletesNothing) {
  SaveMachine(false);
  Launches(5);
  ASSERT_TRUE(RunLogPrefs::SetCollect(Dir(), log_path_, true));
  EXPECT_EQ(Files(), 6);
  EXPECT_EQ(RunLogPrefs::ReadCollect(Dir(), true), std::optional<bool>(true));
  ASSERT_TRUE(RunLogPrefs::SetCollect(Dir(), log_path_, false));
  EXPECT_EQ(Files(), 2);
  EXPECT_EQ(RunLogPrefs::ReadCollect(Dir(), true), std::optional<bool>(false));
}

TEST_F(RunLogPrefsTest, TheSwitchIsNotSavedOverAnUnreadableFile) {
  WriteText(data_dir_ / "machine.json", "{ not json");
  EXPECT_FALSE(RunLogPrefs::SetCollect(Dir(), log_path_, true));
  EXPECT_EQ(ReadAll(data_dir_ / "machine.json"), "{ not json") << "its other settings are not overwritten";
}

TEST_F(RunLogPrefsTest, SavingTheSwitchKeepsTheOtherMachineSettings) {
  MachinePreferences prefs;
  prefs.active_profile_id = "qa";
  prefs.window.width = 999;
  ASSERT_TRUE(UserPreferences::SaveMachine(Dir(), prefs));
  ASSERT_TRUE(RunLogPrefs::SetCollect(Dir(), log_path_, true));
  auto machine = UserPreferences::LoadMachine(Dir());
  ASSERT_TRUE(machine);
  EXPECT_EQ(machine->active_profile_id, "qa");
  EXPECT_EQ(machine->window.width, 999);
  EXPECT_TRUE(machine->collect_run_logs);
}

TEST_F(RunLogPrefsTest, TheCrashSwitchIsOffByDefaultOwnFieldAndGated) {
  EXPECT_FALSE(RunLogPrefs::ReadCollectCrash(Dir(), true));
  ASSERT_TRUE(RunLogPrefs::SetCollectCrash(Dir(), true));
  EXPECT_TRUE(RunLogPrefs::ReadCollectCrash(Dir(), true));
  EXPECT_FALSE(RunLogPrefs::ReadCollectCrash(Dir(), false)) << "gate closed: off whatever is stored";
  auto machine = UserPreferences::LoadMachine(Dir());
  ASSERT_TRUE(machine);
  EXPECT_TRUE(machine->collect_crash_logs);
  EXPECT_FALSE(machine->collect_run_logs) << "not the run-log switch";
}

TEST_F(RunLogPrefsTest, TheMachineDirIsTheRecordedRootElseTheFallback) {
  RunLogPrefs::SetMachineDir("");
  EXPECT_EQ(RunLogPrefs::MachineDir("/session"), "/session");
  RunLogPrefs::SetMachineDir("/default-root");
  EXPECT_EQ(RunLogPrefs::MachineDir("/session"), "/default-root") << "a config override does not move it";
  RunLogPrefs::SetMachineDir("");
}

} // namespace
} // namespace pbr
