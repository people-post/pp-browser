#include "foundation/diagnostics/CrashBreadcrumbs.h"
#include "foundation/diagnostics/CrashDump.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace {

std::string TempDir() {
  const auto base = std::filesystem::temp_directory_path() / "pp-browser-crash-dump-test";
  std::error_code ec;
  std::filesystem::remove_all(base, ec);
  std::filesystem::create_directories(base, ec);
  return base.string();
}

} // namespace

TEST(CrashBreadcrumbs, SnapshotKeepsRecentLines) {
  for (int i = 0; i < 5; ++i) {
    pbr::CrashBreadcrumbs::Append("line-" + std::to_string(i));
  }
  const auto snap = pbr::CrashBreadcrumbs::Snapshot();
  ASSERT_FALSE(snap.empty());
  EXPECT_NE(snap.back().find("line-4"), std::string::npos);
}

TEST(CrashDump, WriteReadClearPending) {
  const std::string dir = TempDir();
  EXPECT_FALSE(pbr::CrashDump::HasPending(dir));

  pbr::CrashBreadcrumbs::Append("before-dump");
  pbr::CrashDump::WritePending(dir, "unit-test");
  ASSERT_TRUE(pbr::CrashDump::HasPending(dir));

  const std::string body = pbr::CrashDump::ReadPending(dir);
  EXPECT_NE(body.find("reason=unit-test"), std::string::npos);
  EXPECT_NE(body.find("before-dump"), std::string::npos);
  EXPECT_NE(body.find("version="), std::string::npos);

  pbr::CrashDump::ClearPending(dir);
  EXPECT_FALSE(pbr::CrashDump::HasPending(dir));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}
