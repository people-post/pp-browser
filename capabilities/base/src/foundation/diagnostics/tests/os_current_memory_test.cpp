#include "foundation/diagnostics/OsCurrentMemory.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

TEST(OsCurrentMemoryTest, BothNumbersArePositiveAndFootprintGrowsWithTouchedMemory) {
  const CurrentMemoryMb before = ReadCurrentMemoryMb();
  EXPECT_GT(before.footprint_mb, 0);
  EXPECT_GT(before.rss_mb, 0);
  std::vector<char> block(128 * 1024 * 1024, 1); // touch 128 MB
  volatile char sink = block[block.size() / 2];
  (void)sink;
  const CurrentMemoryMb after = ReadCurrentMemoryMb();
  EXPECT_GE(after.footprint_mb, before.footprint_mb);
  EXPECT_GE(after.rss_mb, before.rss_mb);
}

} // namespace
} // namespace pbr
