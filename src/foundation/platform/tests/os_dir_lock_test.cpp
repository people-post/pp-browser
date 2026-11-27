#include "foundation/platform/os/OsDirLock.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path FreshDir() {
  const auto dir = std::filesystem::temp_directory_path() /
                   ("pp-dir-lock-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  return dir;
}

} // namespace

// projects/node-relay NR001: one data folder, one process at a time.
TEST(OsDirLockTest, ASecondHolderIsRefusedUntilTheFirstLetsGo) {
  const auto dir = FreshDir();
  const std::string file = (dir / "pp-node.lock").string();
  {
    auto first = pbr::os::OsDirLock::Acquire(file);
    ASSERT_TRUE(static_cast<bool>(first)) << first.error().message;
    auto second = pbr::os::OsDirLock::Acquire(file);
    ASSERT_FALSE(static_cast<bool>(second));
    EXPECT_NE(second.error().message.find(pbr::os::OsDirLock::kHeldElsewhere), std::string::npos);
  }
  auto again = pbr::os::OsDirLock::Acquire(file);
  EXPECT_TRUE(static_cast<bool>(again)) << "released with its holder";
  std::filesystem::remove_all(dir);
}

TEST(OsDirLockTest, AMissingFolderIsAnErrorNotAHold) {
  auto lock = pbr::os::OsDirLock::Acquire((FreshDir() / "no-such-dir" / "pp-node.lock").string());
  ASSERT_FALSE(static_cast<bool>(lock));
  EXPECT_EQ(lock.error().message.find(pbr::os::OsDirLock::kHeldElsewhere), std::string::npos);
}
