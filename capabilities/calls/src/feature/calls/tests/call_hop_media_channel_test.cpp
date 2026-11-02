#include "feature/calls/CallHopMediaChannel.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

namespace pbr {
namespace {

// V057 6: the group path's data plane keeps the publisher streams and the live key ring for its call.
class CallHopMediaChannelTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetSkipDeviceOpenForTest(true);
    channel_ =
        std::make_shared<CallHopMediaChannel>("call:hop", std::make_shared<CallEngine>("call:hop", engine_), relay_);
  }
  void TearDown() override { engine_.Stop(); }

  MediaEngine engine_;
  CallHopMediaRelayDeps relay_;
  std::shared_ptr<CallHopMediaChannel> channel_;
};

TEST_F(CallHopMediaChannelTest, StreamsAreKeptUntilTheHopIsLeft) {
  channel_->SetLocalStreamId(7);
  channel_->AddRemoteStream(11);
  channel_->AddRemoteStream(12);
  channel_->AddRemoteStream(11);
  EXPECT_EQ(channel_->LocalStreamId(), 7u);
  EXPECT_EQ(channel_->RemoteStreamCount(), 2u) << "a stream announced twice is one stream";
  auto remote = channel_->RemoteStreams();
  std::sort(remote.begin(), remote.end());
  EXPECT_EQ(remote, (std::vector<uint32_t>{11, 12}));

  channel_->ResetStreams();
  EXPECT_EQ(channel_->LocalStreamId(), 0u);
  EXPECT_EQ(channel_->RemoteStreamCount(), 0u);
}

TEST_F(CallHopMediaChannelTest, AStreamAsksForItsKeyframeOnce) {
  EXPECT_TRUE(channel_->FirstVideoRefresh(11));
  EXPECT_FALSE(channel_->FirstVideoRefresh(11));
  EXPECT_TRUE(channel_->FirstVideoRefresh(12));
}

TEST_F(CallHopMediaChannelTest, ARotationReachesOnlyALiveRing) {
  channel_->InstallKey(2, ByteVector(32, 0x22)); // no attach yet: nothing to rotate
  EXPECT_FALSE(channel_->HasLiveKeys());
  auto keys = std::make_shared<SfuKeyRing>();
  keys->Install(1, ByteVector(32, 0x11), 1000);
  channel_->MakeLive(keys);
  ASSERT_TRUE(channel_->HasLiveKeys());
  channel_->InstallKey(2, ByteVector(32, 0x22));
  EXPECT_EQ(keys->Current().epoch, 2u) << "the committed attach's ring takes the rotation";
}

} // namespace
} // namespace pbr
