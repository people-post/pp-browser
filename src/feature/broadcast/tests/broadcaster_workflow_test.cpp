#include "feature/broadcast/BroadcasterWorkflow.h"
#include "feature/broadcast/tests/broadcast_test_fakes.h"

#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/VideoCodecUnavailable.h"
#include "domain/mesh/l4/media_relay/client/MediaRelayFrameCrypto.h"
#include "domain/messaging/BroadcastMedia.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

using Phase = BroadcasterWorkflow::Phase;
using test::FakeDial;
using test::FakeRelay;

constexpr const char* kSelf = "12D3KooWPublisher";
constexpr const char* kProgram = "show-9";

class BroadcasterWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    engine_.SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test"); });
    workflow_ = std::make_unique<BroadcasterWorkflow>(Ports());
  }
  void TearDown() override {
    workflow_.reset();
    engine_.Stop();
  }

  BroadcasterPorts Ports() {
    BroadcasterPorts p;
    p.local_peer_id = []() { return std::string(kSelf); };
    p.new_media_key = [this]() { return key_; };
    p.new_join_handle = [](const std::string& program) { return "live:" + program + ":1"; };
    p.put_program_key = [this](const std::string& program, const std::string& join, BroadcastProgramKey key) {
      keys_.push_back({program, join, key});
    };
    p.clear_program_key = [this](const std::string& program, const std::string& join) {
      cleared_.push_back(program + "|" + join);
    };
    p.announce = [this](const BroadcastTipDraft& draft, std::function<void(Roe<void>)> on_done) {
      tips_.push_back(draft);
      on_done(announce_fails_ ? Roe<void>(Error("peer-announce not ready")) : Roe<void>());
    };
    p.relay.relay = &relay_;
    p.relay.dial = &dial_;
    p.engine = &engine_;
    p.post_owner = [this](std::function<void()> task) { ui_.push_back(std::move(task)); };
    p.post_owner_after = [this](std::chrono::milliseconds, std::function<void()> task) { ui_.push_back(std::move(task)); };
    return p;
  }

  void Drain() {
    for (int i = 0; i < 1000 && !ui_.empty(); ++i) {
      auto task = std::move(ui_.front());
      ui_.pop_front();
      task();
    }
  }

  bool WaitFrames(size_t n) {
    for (int i = 0; i < 400; ++i) {
      if (relay_.SentFrames().size() >= n) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  std::vector<BroadcastTipDraft> LiveTips() const {
    std::vector<BroadcastTipDraft> out;
    for (const auto& tip : tips_) {
      if (tip.state == PeerAnnounceState::Live) {
        out.push_back(tip);
      }
    }
    return out;
  }

  struct PutKey {
    std::string program, join;
    BroadcastProgramKey key;
  };

  ByteVector key_ = ByteVector(32, 0x77);
  bool announce_fails_ = false;
  std::vector<PutKey> keys_;
  std::vector<std::string> cleared_;
  std::vector<BroadcastTipDraft> tips_;
  std::deque<std::function<void()>> ui_;
  MediaDeviceArbiter devices_{CreateNullMediaDeviceBackend()};
  CallMediaEngine engine_{devices_};
  FakeDial dial_;
  FakeRelay relay_;
  std::unique_ptr<BroadcasterWorkflow> workflow_;
};

TEST_F(BroadcasterWorkflowTest, GoLivePublishesSealedFramesAndAnnounces) {
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1", "hop2"}}));
  Drain();
  const auto status = workflow_->CurrentStatus();
  ASSERT_EQ(status.phase, Phase::Live) << status.error;
  EXPECT_EQ(status.hop, "hop1");
  EXPECT_EQ(relay_.attached_hop, "hop1");
  EXPECT_EQ(relay_.last_quote.want_down_bps, 0) << "publish-only";
  EXPECT_TRUE(relay_.subscriptions.empty());

  ASSERT_FALSE(keys_.empty());
  EXPECT_EQ(keys_.back().key.hop_peer_id, "hop1") << "tickets point at the carrying hop";
  EXPECT_EQ(keys_.back().key.media_key, key_);
  EXPECT_EQ(keys_.back().key.publisher_peer_id, kSelf);
  const auto live = LiveTips();
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live[0].join_handle, status.join_handle);
  EXPECT_EQ(live[0].hop_peer_id, "hop1");
  EXPECT_EQ(live[0].l1_hop_peer_ids, std::vector<std::string>{"hop2"});

  EXPECT_EQ(engine_.ActiveSpec(), CallMediaEngine::SessionSpec::CaptureOnly());
  EXPECT_TRUE(devices_.Holders(MediaDeviceKind::Speaker).empty()) << "a broadcaster never plays";
  ASSERT_TRUE(WaitFrames(3)) << "capture (silence without a device) is sealed and sent";
  const uint32_t stream = BroadcastPublisherStreamId(kSelf);
  const auto frame = relay_.SentFrames().front();
  EXPECT_EQ(frame.stream_id, stream);
  auto opened = OpenMediaRelayFrame(key_, BroadcastMediaFrameContext(kProgram, status.join_handle), 1, stream,
                                    static_cast<uint8_t>(frame.channel_id), frame.payload);
  EXPECT_TRUE(opened) << "viewers open it with the broadcast label";
  EXPECT_GE(workflow_->CurrentStatus().frames_sent, 3u);
}

TEST_F(BroadcasterWorkflowTest, UnreachableFirstRelayFallsBackAndTheTipNamesTheCarrier) {
  relay_.failing_hops["hop1"] = true;
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1", "hop2"}}));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Live);
  EXPECT_EQ(workflow_->CurrentStatus().hop, "hop2");
  ASSERT_EQ(LiveTips().size(), 1u);
  EXPECT_EQ(LiveTips()[0].hop_peer_id, "hop2");
  EXPECT_EQ(keys_.back().key.hop_peer_id, "hop2");
}

TEST_F(BroadcasterWorkflowTest, NoRelayAcceptsFailsWithoutAnnouncingAndClearsTheKey) {
  relay_.failing_hops["hop1"] = true;
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_NE(workflow_->CurrentStatus().error.find("no media relay accepted"), std::string::npos);
  EXPECT_TRUE(tips_.empty()) << "never announced a show nobody can reach";
  EXPECT_EQ(cleared_.size(), 1u);
  EXPECT_FALSE(engine_.IsActive());
}

TEST_F(BroadcasterWorkflowTest, RelayLossReattachesTheSameHopWithoutReannouncing) {
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1", "hop2"}}));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Live);
  relay_.Lose();
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Live);
  EXPECT_EQ(workflow_->CurrentStatus().hop, "hop1");
  EXPECT_EQ(workflow_->CurrentStatus().reattaches, 1);
  EXPECT_EQ(LiveTips().size(), 1u) << "same hop: the tip is still right";
  EXPECT_TRUE(engine_.IsActive()) << "capture survives a re-attach";
}

TEST_F(BroadcasterWorkflowTest, LossOnADeadHopMovesAndReannouncesTheNewHop) {
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1", "hop2"}}));
  Drain();
  relay_.failing_hops["hop1"] = true;
  relay_.Lose();
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Live);
  EXPECT_EQ(workflow_->CurrentStatus().hop, "hop2");
  const auto live = LiveTips();
  ASSERT_EQ(live.size(), 2u);
  EXPECT_EQ(live.back().hop_peer_id, "hop2");
  EXPECT_EQ(keys_.back().key.hop_peer_id, "hop2");
}

TEST_F(BroadcasterWorkflowTest, EndAnnouncesEndedClearsTheKeyAndStopsSending) {
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  Drain();
  ASSERT_TRUE(WaitFrames(1));
  const std::string join = workflow_->CurrentStatus().join_handle;
  workflow_->End();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Idle);
  ASSERT_FALSE(tips_.empty());
  EXPECT_EQ(tips_.back().state, PeerAnnounceState::Ended);
  EXPECT_EQ(tips_.back().join_handle, join);
  EXPECT_EQ(cleared_, std::vector<std::string>{std::string(kProgram) + "|" + join});
  EXPECT_FALSE(relay_.attached.load());
  EXPECT_FALSE(engine_.IsActive());
  EXPECT_TRUE(relay_.observers.empty());
  const size_t after = relay_.SentFrames().size();
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_EQ(relay_.SentFrames().size(), after);
}

TEST_F(BroadcasterWorkflowTest, RelayHeldByACallIsRefusedClearly) {
  relay_.attached = true;
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_EQ(workflow_->CurrentStatus().error, "media relay client in use by a call");
  EXPECT_EQ(relay_.detaches, 0);
}

TEST_F(BroadcasterWorkflowTest, LateAttachAfterEndIsDetached) {
  relay_.hold_attach = true;
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  Drain();
  workflow_->End();
  relay_.held();
  Drain();
  EXPECT_FALSE(relay_.attached.load());
  EXPECT_TRUE(tips_.empty());
}

TEST_F(BroadcasterWorkflowTest, UnpublishableTipFailsTheGoLive) {
  announce_fails_ = true;
  ASSERT_TRUE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_NE(workflow_->CurrentStatus().error.find("live tip not published"), std::string::npos);
  EXPECT_FALSE(relay_.attached.load());
}

TEST_F(BroadcasterWorkflowTest, UnusableRequestsAreRefused) {
  EXPECT_FALSE(workflow_->GoLive({"topic", "", {"hop1"}}));
  EXPECT_FALSE(workflow_->GoLive({"topic", kProgram, {}}));
  key_ = ByteVector(16, 1);
  EXPECT_FALSE(workflow_->GoLive({"topic", kProgram, {"hop1"}}));
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Idle);
}

} // namespace
} // namespace pbr
