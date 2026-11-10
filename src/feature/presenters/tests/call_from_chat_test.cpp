// One way a chat calls (CallPresenter::CallFromChat): the call button, a tapped call record (#164) and
// "Call {name} back?" (V064) join a call that goes on, drop to voice without video, else start as asked.
#include "feature/presenters/CallPresenter.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(CallFromChatTest, VideoWhenAvailableAsksForVideo) {
  EXPECT_TRUE(ChatCallAsksVideo(/*call_joinable=*/false, /*want_video=*/true, /*video_available=*/true));
}

TEST(CallFromChatTest, VideoWithoutSupportGoesVoice) {
  EXPECT_FALSE(ChatCallAsksVideo(false, true, /*video_available=*/false));
}

TEST(CallFromChatTest, VoiceStaysVoice) {
  EXPECT_FALSE(ChatCallAsksVideo(false, /*want_video=*/false, true));
}

TEST(CallFromChatTest, AJoinableCallIsJoinedAsItIsNotAsVideo) {
  EXPECT_FALSE(ChatCallAsksVideo(/*call_joinable=*/true, true, true));
}

// No chat, or no calls on this device: nothing is asked, and no error is shown for a call nobody placed.
TEST(CallFromChatTest, NothingWithoutAChat) {
  CallPresenter presenter;
  int notices = 0;
  int pickers = 0;
  CallPresenter::Callbacks callbacks;
  callbacks.notify = [&](const CallNotice&) { ++notices; };
  callbacks.open_group_picker = [&](const std::string&, bool) { ++pickers; };
  presenter.BindCallbacks(std::move(callbacks));
  EXPECT_FALSE(presenter.CallFromChat("", true));
  EXPECT_EQ(notices, 0);
  EXPECT_EQ(pickers, 0);
}

// Without the calls stack the ask fails the way the call button does ("calls unavailable"), once.
TEST(CallFromChatTest, WithoutTheCallsStackItFailsAsTheCallButtonDoes) {
  CallPresenter presenter;
  CallStartResult got;
  int answers = 0;
  EXPECT_FALSE(presenter.CallFromChat("thread-1", true, [&](const CallStartResult& result) {
    got = result;
    ++answers;
  }));
  EXPECT_EQ(answers, 1);
  EXPECT_EQ(got.kind, CallStartResult::Kind::Failed);
  EXPECT_EQ(got.error_key, "call.error.unavailable");
}

} // namespace
} // namespace pbr
