#include <gtest/gtest.h>

#include "foundation/product/BuildInfo.h"

namespace pbr {

TEST(BuildInfoTest, TagComesBeforeCommit) {
  EXPECT_EQ(FormatBuildLabel("a1b2c3d4", "internal/141"), "internal/141 \xC2\xB7 a1b2c3d4");
}

TEST(BuildInfoTest, NoTagShowsOnlyTheCommit) {
  EXPECT_EQ(FormatBuildLabel("a1b2c3d4", ""), "a1b2c3d4");
}

TEST(BuildInfoTest, DirtyCommitIsKept) {
  EXPECT_EQ(FormatBuildLabel("a1b2c3d4-dirty", ""), "a1b2c3d4-dirty");
  EXPECT_EQ(FormatBuildLabel("a1b2c3d4-dirty", "internal/141"), "internal/141 \xC2\xB7 a1b2c3d4-dirty");
}

TEST(BuildInfoTest, UnknownCommitIsShownAsUnknown) {
  EXPECT_EQ(FormatBuildLabel("unknown", ""), "unknown");
}

TEST(BuildInfoTest, ThisBuildHasACommit) {
  const std::string commit = BuildCommit();
  EXPECT_FALSE(commit.empty());
  EXPECT_EQ(BuildLabel(), FormatBuildLabel(BuildCommit(), BuildTag()));
}

} // namespace pbr
