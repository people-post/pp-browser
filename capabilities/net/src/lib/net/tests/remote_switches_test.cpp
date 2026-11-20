#include "lib/net/RemoteSwitches.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

// O2: remote switches from the org relay's client-compat, read once a run. No product switch is defined in release 1,
// so these names are the test's own.
class RemoteSwitchesTest : public ::testing::Test {
protected:
  void SetUp() override { RemoteSwitches::ResetForTest(); }
  void TearDown() override { RemoteSwitches::ResetForTest(); }
};

TEST_F(RemoteSwitchesTest, AnAbsentSwitchIsItsBuiltInDefault) {
  EXPECT_FALSE(RemoteSwitches::Loaded());
  EXPECT_TRUE(RemoteSwitches::IsOn("test.anything", true)) << "no document yet: the default";
  EXPECT_FALSE(RemoteSwitches::IsOn("test.anything", false));
  RemoteSwitches::Load({{"test.other", true}});
  EXPECT_TRUE(RemoteSwitches::Loaded());
  EXPECT_TRUE(RemoteSwitches::IsOn("test.anything", true)) << "the document does not name it: the default";
  EXPECT_FALSE(RemoteSwitches::IsOn("test.anything", false));
}

TEST_F(RemoteSwitchesTest, ANamedSwitchOverridesTheDefaultEitherWay) {
  RemoteSwitches::Load({{"test.on", true}, {"test.off", false}});
  EXPECT_TRUE(RemoteSwitches::IsOn("test.on", false));
  EXPECT_FALSE(RemoteSwitches::IsOn("test.off", true));
}

TEST_F(RemoteSwitchesTest, TheRunKeepsItsFirstDocumentsSwitches) {
  RemoteSwitches::Load({{"test.flag", true}});
  RemoteSwitches::Load({{"test.flag", false}}); // a later fetch: taken at the next start, not now
  EXPECT_TRUE(RemoteSwitches::IsOn("test.flag", false));
}

} // namespace
} // namespace pbr
