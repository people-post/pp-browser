#include "feature/ui/CallConflictCopy.h"

#include <gtest/gtest.h>

TEST(CallConflictCopyTest, NoConflictUsesAcceptDecline) {
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(false, false, "Alice", "Bob");
  EXPECT_EQ(copy.eyebrow, "Incoming call");
  EXPECT_EQ(copy.accept_label, "Accept");
  EXPECT_EQ(copy.decline_label, "Decline");
  EXPECT_TRUE(copy.hint.empty());
}

TEST(CallConflictCopyTest, SamePeerCallbackHint) {
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, true, "Alice", "Alice");
  EXPECT_EQ(copy.eyebrow, "You're already calling");
  EXPECT_EQ(copy.accept_label, "End & Accept");
  EXPECT_EQ(copy.decline_label, "Ignore");
  EXPECT_EQ(copy.hint, "Alice is calling you back. Answering ends your outgoing call.");
}

TEST(CallConflictCopyTest, DifferentPeerEndsCurrentCall) {
  const pbr::CallConflictCopy copy = pbr::MakeCallConflictCopy(true, false, "Carol", "Bob");
  EXPECT_EQ(copy.accept_label, "End & Accept");
  EXPECT_EQ(copy.decline_label, "Ignore");
  EXPECT_EQ(copy.hint, "Answering will end your current call with Bob.");
}
