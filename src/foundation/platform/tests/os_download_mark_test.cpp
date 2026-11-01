#include "foundation/platform/os/OsDownloadMark.h"

#include "common/Utilities.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#ifdef __APPLE__
#include <sys/xattr.h>
#endif

namespace pbr {
namespace {

// A file from someone else is marked as downloaded from the internet: macOS asks before opening it.
TEST(OsDownloadMarkTest, AMarkedFileCarriesTheQuarantineAttribute) {
#ifdef __APPLE__
  const auto dir = std::filesystem::temp_directory_path() / ("pp_download_mark_" + util::GenerateUuid());
  std::filesystem::create_directories(dir);
  const auto path = dir / "a.doc";
  std::ofstream(path) << "hello";
  ASSERT_TRUE(os::MarkFileAsDownloaded(path.string()));
  char buf[256] = {};
  const ssize_t n = ::getxattr(path.c_str(), "com.apple.quarantine", buf, sizeof(buf), 0, 0);
  ASSERT_GT(n, 0) << "quarantine xattr missing";
  EXPECT_EQ(std::string(buf, static_cast<size_t>(n)).rfind("0081;", 0), 0u);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
#else
  GTEST_SKIP() << "macOS only";
#endif
}

} // namespace
} // namespace pbr
