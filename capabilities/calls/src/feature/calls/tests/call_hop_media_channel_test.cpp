#include "feature/calls/CallHopMediaChannel.h"

#include "domain/call_media/CallMediaFrameCrypto.h"
#include "mesh/l4/shared/MediaFrame.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

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

// --- V067: which frames say a publisher is alive ------------------------------------------------------

class HopFrameLivenessTest : public CallHopMediaChannelTest {
protected:
  void SetUp() override {
    CallHopMediaChannelTest::SetUp();
    keys_->Install(1, key_, 1000);
    sink_ = channel_->MakeFrameSink(keys_, std::make_shared<std::atomic<bool>>(true));
    channel_->AddRemoteStream(11);
  }
  /** A frame of stream 11 as its publisher sealed it under `key`. */
  mesh::MediaDataFrame Sealed(const uint32_t seq, const ByteVector& key, const uint32_t stream = 11) {
    mesh::MediaDataFrame frame;
    frame.stream_id = stream;
    frame.channel_id = 0;
    frame.seq = seq;
    frame.instance = 0xAA;
    frame.capture_ts = seq * 40;
    EXPECT_TRUE(mesh::SealMediaFrame(key, CallMediaSfuContext("call:hop"), 1, frame, std::vector<uint8_t>{1, 2, 3}));
    return frame;
  }
  void Arrives(const mesh::MediaDataFrame& frame) {
    std::this_thread::sleep_for(std::chrono::milliseconds(3)); // a later wall ms than the frame before
    sink_(frame);
  }

  ByteVector key_ = ByteVector(32, 0x11);
  std::shared_ptr<SfuKeyRing> keys_ = std::make_shared<SfuKeyRing>();
  std::function<void(mesh::MediaDataFrame)> sink_;
};

TEST_F(HopFrameLivenessTest, AFrameThatOpensAndAdvancesSaysItsPublisherIsAlive) {
  EXPECT_EQ(channel_->LastFreshFrameMs(11), 0) << "nothing heard yet";
  Arrives(Sealed(5, key_));
  const int64_t first = channel_->LastFreshFrameMs(11);
  EXPECT_GT(first, 0);
  Arrives(Sealed(6, key_));
  EXPECT_GT(channel_->LastFreshFrameMs(11), first);
}

TEST_F(HopFrameLivenessTest, AReplayedOrOldFrameSaysNothing) {
  const mesh::MediaDataFrame frame = Sealed(5, key_);
  Arrives(frame);
  const int64_t heard = channel_->LastFreshFrameMs(11);
  Arrives(frame); // the hop sends it again
  Arrives(Sealed(4, key_));
  EXPECT_EQ(channel_->LastFreshFrameMs(11), heard) << "the hop can repeat a frame, not make one";
}

TEST_F(HopFrameLivenessTest, AFrameThatDoesNotOpenSaysNothing) {
  Arrives(Sealed(5, ByteVector(32, 0x99))); // not the call's key: the hop (or anyone) made it up
  EXPECT_EQ(channel_->LastFreshFrameMs(11), 0);
}

TEST_F(HopFrameLivenessTest, AStreamNotWantedSaysNothing) {
  Arrives(Sealed(5, key_, /*stream=*/12)); // never subscribed
  EXPECT_EQ(channel_->LastFreshFrameMs(12), 0);
  Arrives(Sealed(5, key_));
  ASSERT_GT(channel_->LastFreshFrameMs(11), 0);
  channel_->DropRemoteStream(11); // its publisher left
  EXPECT_EQ(channel_->LastFreshFrameMs(11), 0);
  EXPECT_EQ(channel_->RemoteStreamCount(), 0u);
  Arrives(Sealed(6, key_));
  EXPECT_EQ(channel_->LastFreshFrameMs(11), 0) << "what the hop still forwards of it is dropped";
  EXPECT_TRUE(channel_->FirstVideoRefresh(11)) << "a rejoin asks for its keyframe again";
}

} // namespace
} // namespace pbr
