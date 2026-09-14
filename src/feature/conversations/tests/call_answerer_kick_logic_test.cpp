#include "feature/calls/CallAnswererKickLogic.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TEST(CallAnswererKickLogicTest, KicksWhenDirectArmedAndPeerKnown) {
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = true;
  in.peer_nonempty = true;
  EXPECT_TRUE(ShouldKickAnswererDirectMedia(in));
}

TEST(CallAnswererKickLogicTest, SkipsWhenStatusDisallowsBridge) {
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = false;
  in.peer_nonempty = true;
  EXPECT_FALSE(ShouldKickAnswererDirectMedia(in));
}

TEST(CallAnswererKickLogicTest, SkipsWhenMediaLive) {
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = true;
  in.media_live_same_call = true;
  in.peer_nonempty = true;
  EXPECT_FALSE(ShouldKickAnswererDirectMedia(in));
}

TEST(CallAnswererKickLogicTest, KicksWhenActiveButDeadCapture) {
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = true;
  in.media_live_same_call = false; // IsActive but tx=0 / not connected
  in.peer_nonempty = true;
  EXPECT_TRUE(ShouldKickAnswererDirectMedia(in));
}

TEST(CallAnswererKickLogicTest, SkipsWhenNoPeer) {
  CallAnswererKickDecisionInput in;
  in.allows_direct_path = true;
  EXPECT_FALSE(ShouldKickAnswererDirectMedia(in));
}

} // namespace
} // namespace pbr
