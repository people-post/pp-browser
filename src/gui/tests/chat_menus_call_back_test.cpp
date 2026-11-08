// The call button and a tapped call record share StartCallWith: join, voice downgrade, else start as asked.
#include "gui/chat/ChatMenus.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

struct Started {
  std::string thread;
  bool video;
};

class StartCallWithTest : public ::testing::Test {
protected:
  CallActionsPorts Ports(bool video_available) {
    CallActionsPorts ports;
    ports.start_call = [this](const std::string& thread, bool video) {
      calls_.push_back({thread, video});
      return true;
    };
    ports.video_call_available = [video_available] { return video_available; };
    return ports;
  }
  std::vector<Started> calls_;
};

TEST_F(StartCallWithTest, VideoWhenAvailableStartsOneVideoCall) {
  EXPECT_TRUE(StartCallWith(Ports(true), "t1", false, true));
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_EQ(calls_[0].thread, "t1");
  EXPECT_TRUE(calls_[0].video);
}

TEST_F(StartCallWithTest, VideoWithoutSupportGoesVoice) {
  EXPECT_TRUE(StartCallWith(Ports(false), "t1", false, true));
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_FALSE(calls_[0].video);
}

TEST_F(StartCallWithTest, VoiceStaysVoice) {
  EXPECT_TRUE(StartCallWith(Ports(true), "t1", false, false));
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_FALSE(calls_[0].video);
}

TEST_F(StartCallWithTest, AJoinableCallIsJoinedNotStartedAsVideo) {
  EXPECT_TRUE(StartCallWith(Ports(true), "t1", true, true));
  ASSERT_EQ(calls_.size(), 1u);
  EXPECT_FALSE(calls_[0].video);  // join flag path: start_call(thread, false) joins the call that goes on
}

TEST_F(StartCallWithTest, NothingWithoutAThreadOrAPort) {
  EXPECT_FALSE(StartCallWith(Ports(true), "", false, true));
  CallActionsPorts none;
  EXPECT_FALSE(StartCallWith(none, "t1", false, true));
  EXPECT_TRUE(calls_.empty());
}

} // namespace
} // namespace pbr
