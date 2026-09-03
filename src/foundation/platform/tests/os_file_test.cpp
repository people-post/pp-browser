#include "foundation/platform/os/OsFile.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace {

std::filesystem::path TempDir() {
  return std::filesystem::temp_directory_path() / "pp-browser-os-file-test";
}

} // namespace

TEST(OsFileTest, AtomicRenameRoundTrip) {
  const auto dir = TempDir();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);

  const auto tmp = dir / "payload.tmp";
  const auto final_path = dir / "payload.txt";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.is_open());
    out << "hello";
  }

  ASSERT_TRUE(pbr::os::FsyncFile(tmp));
  ASSERT_TRUE(pbr::os::AtomicRename(tmp, final_path));

  std::ifstream in(final_path);
  std::string contents;
  std::getline(in, contents);
  EXPECT_EQ(contents, "hello");

  std::filesystem::remove_all(dir, ec);
}

TEST(OsFileTest, GetPidIsNonZero) {
  EXPECT_GT(pbr::os::GetPid(), 0u);
}
