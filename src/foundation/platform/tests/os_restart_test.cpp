// The pure parts of restarting the program after Reset my profile: the start arguments are kept as they came, the plan
// carries them and the program file byte for byte. The process start itself (execve / posix_spawn / CreateProcessW) cannot be unit-tested: it
// replaces or duplicates the test process.
#include "foundation/platform/os/OsRestart.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(OsRestartTest, TheStartArgumentsAreKeptByteForByteAndOnlyTheFirstCallCounts) {
  std::string a0 = "/Applications/PP 测试.app/Contents/MacOS/PP";
  std::string a1 = "--log-file";
  std::string a2 = "/tmp/my log \"quoted\".log";
  std::string a3 = "";
  std::vector<char*> argv{a0.data(), a1.data(), a2.data(), a3.data(), nullptr};
  os::CaptureStartArguments(4, argv.data());
  const std::vector<std::string> expected{a0, a1, a2, a3};
  EXPECT_EQ(os::StartArguments(), expected);

  std::string other = "/other";
  std::vector<char*> later{other.data(), nullptr};
  os::CaptureStartArguments(1, later.data());
  EXPECT_EQ(os::StartArguments(), expected) << "a later call changes nothing";
}

TEST(OsRestartTest, ThePlanHasTheProgramFileAndTheArgumentsAsTheyWere) {
  const std::vector<std::string> args{"pp", "--debug", "a b", "c\"d", "--log-file=/x y/z.log"};
  const os::RestartPlan plan = os::PlanRestart("/real/path/to/pp", args);
  ASSERT_TRUE(plan.Ready());
  EXPECT_EQ(plan.exe, "/real/path/to/pp") << "the resolved file, whatever argv[0] says";
  EXPECT_EQ(plan.argv, args);
}

TEST(OsRestartTest, WithoutAProgramFileOrArgumentsThereIsNoPlan) {
  EXPECT_FALSE(os::PlanRestart("", {"pp"}).Ready());
  EXPECT_FALSE(os::PlanRestart("/pp", {}).Ready());
}

TEST(OsRestartTest, AnUnreadyPlanIsNotStarted) {
  std::string error;
  EXPECT_FALSE(os::RestartProcess(os::RestartPlan{}, error));
  EXPECT_FALSE(error.empty());
}

} // namespace
} // namespace pbr
