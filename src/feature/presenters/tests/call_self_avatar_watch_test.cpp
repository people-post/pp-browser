#include "feature/presenters/CallSelfAvatarWatch.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallSelfAvatarWatchTest, NothingChangesWhileThereIsNoIdentityAndTheUnlockIsTheOneChange) {
  CallSelfAvatarWatch watch;
  EXPECT_FALSE(watch.Changed("")) << "locked: nothing to read, no refresh per frame";
  EXPECT_TRUE(watch.Changed("acct-1")) << "unlocked";
  watch.Remember("acct-1");
  for (int frame = 0; frame < 100; ++frame) {
    EXPECT_FALSE(watch.Changed("acct-1")) << "the same identity is not read again";
  }
  EXPECT_TRUE(watch.Changed("acct-2"));
}

} // namespace
} // namespace pbr
