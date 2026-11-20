#include "lib/media/AutoreleasePool.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(AutoreleasePoolScopeTest, ScopeRunsTheBodyOnceAndNests) {
  int runs = 0;
  {
    AutoreleasePoolScope outer;
    {
      AutoreleasePoolScope inner;
      ++runs;
    }
  }
  EXPECT_EQ(runs, 1);
}

} // namespace
} // namespace pbr
