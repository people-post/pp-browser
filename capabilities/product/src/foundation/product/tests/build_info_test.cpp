#include <gtest/gtest.h>

#include "foundation/product/BuildInfo.h"

#include "foundation/product/AppVersion.h"

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

TEST(BuildInfoTest, IdentityNamesVersionTagAndCommit) {
  EXPECT_EQ(FormatBuildIdentity("1.0.0", "a1b2c3d4", "v1.0.0-rc.3"), "1.0.0 \xC2\xB7 v1.0.0-rc.3 \xC2\xB7 a1b2c3d4");
}

TEST(BuildInfoTest, IdentityWithoutTagNamesVersionAndCommit) {
  EXPECT_EQ(FormatBuildIdentity("1.0.0", "a1b2c3d4", ""), "1.0.0 \xC2\xB7 a1b2c3d4");
}

TEST(BuildInfoTest, IdentityWithUnknownCommitSaysUnknown) {
  EXPECT_EQ(FormatBuildIdentity("1.0.0", "unknown", ""), "1.0.0 \xC2\xB7 unknown");
  EXPECT_EQ(FormatBuildIdentity("1.0.0", "unknown", "internal/141"), "1.0.0 \xC2\xB7 internal/141 \xC2\xB7 unknown");
}

// Release-compatibility RC022: a tester build names itself in About with its TestFlight build number; a store build
// shows no such line.
TEST(BuildInfoTest, ATesterBuildShowsItsTestFlightBuildNumberAndAStoreBuildNothing) {
  EXPECT_EQ(TesterBuildNumber(true, "internal/141", "1.0.0"), "141");
  EXPECT_EQ(TesterBuildNumber(true, "", "1.0.0"), "1.0.0") << "no tag: the version";
  EXPECT_EQ(TesterBuildNumber(true, "v1.0.0-rc.3", "1.0.0"), "1.0.0") << "not an internal/<n> tag";
  EXPECT_EQ(TesterBuildNumber(true, "internal/", "1.0.0"), "1.0.0");
  EXPECT_EQ(TesterBuildNumber(true, "internal/14a", "1.0.0"), "1.0.0");
  for (const char* tag : {"internal/141", "", "v1.0.0-rc.3"}) {
    EXPECT_EQ(TesterBuildNumber(false, tag, "1.0.0"), "") << tag;
  }
}

TEST(BuildInfoTest, ThisBuildHasACommit) {
  const std::string commit = BuildCommit();
  EXPECT_FALSE(commit.empty());
  EXPECT_EQ(BuildLabel(), FormatBuildLabel(BuildCommit(), BuildTag()));
  EXPECT_EQ(BuildIdentity(), FormatBuildIdentity(AppVersionString(), BuildCommit(), BuildTag()));
}

} // namespace pbr
