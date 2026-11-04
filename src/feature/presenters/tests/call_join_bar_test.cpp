// A group chat's "a call goes on here" bar (DEV/PLAN-ui-batch8-group-pages.md 4): shown while a call may be joined,
// voice or video from the call's mode, the number of people only when the participant table gives one (never "0").
#include "feature/presenters/CallPresenter.h"

#include <gtest/gtest.h>

using namespace pbr;

namespace {

CallUiBackend::JoinableCallInfo Call(const CallMediaMode mode, const std::optional<int> joined) {
  CallUiBackend::JoinableCallInfo info;
  info.call_id = "c1";
  info.media_mode = mode;
  info.joined_count = joined;
  return info;
}

} // namespace

TEST(CallJoinBarTest, NothingToJoinNoBar) {
  const CallJoinBar bar = MakeCallJoinBar(std::nullopt);
  EXPECT_FALSE(bar.shown);
  EXPECT_TRUE(bar.text_key.empty());
}

TEST(CallJoinBarTest, AVoiceCallWithItsPeople) {
  const CallJoinBar bar = MakeCallJoinBar(Call(CallMediaMode::Voice, 3));
  EXPECT_TRUE(bar.shown);
  EXPECT_FALSE(bar.video);
  EXPECT_EQ(bar.text_key, "chat.call_bar.voice");
  EXPECT_EQ(bar.joined, 3);
}

TEST(CallJoinBarTest, AVideoCallSaysVideo) {
  const CallJoinBar bar = MakeCallJoinBar(Call(CallMediaMode::Video, 2));
  EXPECT_TRUE(bar.video);
  EXPECT_EQ(bar.text_key, "chat.call_bar.video");
}

TEST(CallJoinBarTest, NoNumberWhenUnknownAndNeverZero) {
  EXPECT_EQ(MakeCallJoinBar(Call(CallMediaMode::Voice, std::nullopt)).joined, 0);
  EXPECT_EQ(MakeCallJoinBar(Call(CallMediaMode::Voice, 0)).joined, 0);
  EXPECT_TRUE(MakeCallJoinBar(Call(CallMediaMode::Voice, 0)).shown);
}
