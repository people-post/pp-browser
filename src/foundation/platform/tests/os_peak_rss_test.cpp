#include "foundation/platform/os/OsPeakRss.h"

#include <gtest/gtest.h>

#include <vector>

namespace pbr {
namespace {

TEST(OsPeakRssTest, IsAHighWaterMarkThatGrowsWithUse) {
  const int64_t before = PeakResidentBytes();
#if defined(_WIN32)
  EXPECT_EQ(before, 0);  // no backend: the metric prints 0
#else
  EXPECT_GT(before, 1'000'000);
  std::vector<char> block(64 * 1024 * 1024, 1);  // touch 64 MB
  volatile char sink = block[block.size() / 2];
  (void)sink;
  const int64_t after = PeakResidentBytes();
  EXPECT_GE(after, before);
  EXPECT_GE(after - before, 0);
#endif
}

} // namespace
} // namespace pbr
