#include "feature/broadcast/BroadcastHub.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"

#include "domain/media/VideoCodecUnavailable.h"
#include "domain/mesh/l4/media_relay/MediaRelayFrameCrypto.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/BroadcastMedia.h"
#include "foundation/crypto/MlDsa.h"
#include "feature/broadcast/tests/broadcast_test_fakes.h"

#include <gtest/gtest.h>
#include <opus.h>

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pbr {
namespace {

using test::FakeDial;
using test::FakeRelay;
using test::OpusFrame;

using Phase = BroadcastViewerWorkflow::Phase;

constexpr const char* kPublisher = "12D3KooWPublisher";
constexpr const char* kViewer = "12D3KooWViewer";
constexpr const char* kProgram = "show-1";
constexpr const char* kJoin = "live:show-1";
constexpr int64_t kNow = 1'900'000'000'000;

class BroadcastViewerWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto keys = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(keys);
    publisher_keys_ = *keys;
    media_key_.assign(32, 0x5a);
    engine_.SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test"); });
    workflow_ = std::make_unique<BroadcastViewerWorkflow>(Ports());
  }
  void TearDown() override {
    workflow_.reset();
    engine_.Stop();
  }

  BroadcastViewerPorts Ports() {
    BroadcastViewerPorts p;
    p.local_peer_id = []() { return std::string(kViewer); };
    p.publisher_key = [this](const std::string& peer) -> std::optional<ByteVector> {
      return peer == kPublisher ? std::optional<ByteVector>(verify_key_ ? *verify_key_ : publisher_keys_.public_key)
                                : std::nullopt;
    };
    p.reach_peer = [this](const std::string& peer, std::function<void(Roe<void>)> done) {
      reached_.push_back(peer);
      done(publisher_reachable_ ? Roe<void>() : Roe<void>(Error("no link")));
    };
    p.request_ticket = [this](const std::string& /*publisher*/, const BroadcastTicketRequest& req,
                              std::function<void(Roe<BroadcastTicketResponse>)> done) {
      ticket_requests_.push_back(req);
      BroadcastTicketResponse response;
      if (refuse_ticket_) {
        response.error = "not live";
        done(response);
        return;
      }
      BroadcastJoinTicketDraft draft;
      draft.publisher_peer_id = kPublisher;
      draft.program_id = kProgram;
      draft.join_handle = kJoin;
      draft.viewer_peer_id = req.viewer_peer_id;
      draft.hop_peer_id = ticket_hop_;
      draft.expires_at_ms = kNow + 60'000;
      auto ticket = MintBroadcastJoinTicket(draft, media_key_, publisher_keys_.secret_key);
      ASSERT_TRUE(ticket) << ticket.error().message;
      response.ok = true;
      response.ticket = *ticket;
      done(response);
    };
    p.request_admission = [this](const std::string& hop, const BroadcastViewerAttachRequest& req,
                                 std::function<void(Roe<BroadcastViewerAttachResult>)> done) {
      admissions_.emplace_back(hop, req);
      auto it = admission_.find(hop);
      if (it == admission_.end()) {
        done(Error("protocol not supported"));
        return;
      }
      done(it->second);
    };
    p.relay.relay = &relay_;
    p.relay.dial = &dial_;
    p.engine = &engine_;
    p.post_ui = [this](std::function<void()> task) { ui_.push_back(std::move(task)); };
    p.post_ui_after = [this](std::chrono::milliseconds, std::function<void()> task) { ui_.push_back(std::move(task)); };
    p.now_ms = []() { return kNow; };
    return p;
  }

  void Drain() {
    for (int i = 0; i < 1000 && !ui_.empty(); ++i) {
      auto task = std::move(ui_.front());
      ui_.pop_front();
      task();
    }
  }

  static BroadcastWatchTarget Target(std::vector<std::string> hops) {
    return BroadcastWatchTarget{kPublisher, kProgram, kJoin, std::move(hops)};
  }
  static BroadcastViewerAttachResult Admit(const std::string& hop) {
    BroadcastViewerAttachResult r;
    r.action = BroadcastLadderViewerAction::Admit;
    r.admitted_hop_peer_id = hop;
    return r;
  }
  static BroadcastViewerAttachResult Redirect(std::vector<std::string> to) {
    BroadcastViewerAttachResult r;
    r.action = BroadcastLadderViewerAction::Redirect;
    r.redirect_peer_ids = std::move(to);
    r.redirect_budget_remaining = 7;
    return r;
  }
  static BroadcastViewerAttachResult Refuse(std::string reason) {
    BroadcastViewerAttachResult r;
    r.action = BroadcastLadderViewerAction::Refuse;
    r.refuse_reason = std::move(reason);
    return r;
  }

  MediaDataFrame SealedFrame(const std::string& context, uint32_t seq,
                             uint32_t stream = BroadcastPublisherStreamId(kPublisher)) {
    auto body = SealMediaRelayFrame(media_key_, context, 1, stream, seq, 0, 0, opus_);
    EXPECT_TRUE(body);
    MediaDataFrame f;
    f.stream_id = stream;
    f.channel_id = 0;
    f.seq = seq;
    f.payload = *body;
    return f;
  }

  const std::string context_ = BroadcastMediaFrameContext(kProgram, kJoin);
  const std::vector<uint8_t> opus_ = OpusFrame();
  MlDsaKeyPair publisher_keys_;
  std::optional<ByteVector> verify_key_;
  ByteVector media_key_;
  bool publisher_reachable_ = true;
  bool refuse_ticket_ = false;
  std::string ticket_hop_;
  std::map<std::string, Roe<BroadcastViewerAttachResult>> admission_;
  std::vector<std::string> reached_;
  std::vector<BroadcastTicketRequest> ticket_requests_;
  std::vector<std::pair<std::string, BroadcastViewerAttachRequest>> admissions_;
  std::deque<std::function<void()>> ui_;
  MediaDeviceArbiter devices_{CreateNullMediaDeviceBackend()};
  CallMediaEngine engine_{devices_};
  FakeDial dial_;
  FakeRelay relay_;
  std::unique_ptr<BroadcastViewerWorkflow> workflow_;
};

TEST_F(BroadcastViewerWorkflowTest, AdmittedViewerListensReceiveOnlyOnThePublisherStream) {
  admission_.emplace("hop1", Admit("hop1"));
  ASSERT_TRUE(workflow_->Watch(Target({"hop1"})));
  Drain();
  const auto& status = workflow_->CurrentStatus();
  ASSERT_EQ(status.phase, Phase::Listening) << status.error;
  EXPECT_EQ(status.hop, "hop1");
  EXPECT_EQ(reached_, std::vector<std::string>{kPublisher}) << "ticket fetched from the publisher";
  ASSERT_EQ(ticket_requests_.size(), 1u);
  EXPECT_EQ(ticket_requests_[0].viewer_peer_id, kViewer);
  ASSERT_EQ(admissions_.size(), 1u);
  EXPECT_FALSE(admissions_[0].second.ticket_json.empty()) << "hop verifies the ticket";
  EXPECT_EQ(relay_.last_session, kJoin);
  EXPECT_EQ(relay_.last_quote.want_up_bps, 0) << "receive-only";
  EXPECT_EQ(relay_.subscriptions,
            (std::vector<std::pair<uint32_t, uint16_t>>{{BroadcastPublisherStreamId(kPublisher), 0}}));
  EXPECT_EQ(engine_.ActiveSpec(), CallMediaEngine::SessionSpec::PlaybackOnly());
  EXPECT_TRUE(devices_.Holders(MediaDeviceKind::Mic).empty()) << "a viewer never takes the mic";

  relay_.sink(SealedFrame(context_, 1));
  relay_.sink(SealedFrame(context_, 2));
  EXPECT_GE(engine_.HealthSnapshot().rx_audio_frames, 2u);
  std::this_thread::sleep_for(std::chrono::milliseconds(60));
  EXPECT_TRUE(relay_.SentFrames().empty()) << "a viewer never publishes";
}

TEST_F(BroadcastViewerWorkflowTest, FramesUnderAnotherLabelOrStreamNeverReachTheEngine) {
  admission_.emplace("hop1", Admit("hop1"));
  ASSERT_TRUE(workflow_->Watch(Target({"hop1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  relay_.sink(SealedFrame("call-media-sfu|" + std::string(kJoin), 1));
  relay_.sink(SealedFrame(context_, 2, /*stream=*/4242));
  EXPECT_EQ(engine_.HealthSnapshot().rx_audio_frames, 0u);
}

TEST_F(BroadcastViewerWorkflowTest, RedirectIsFollowedWithAPathStamp) {
  admission_.emplace("l1", Redirect({"child"}));
  admission_.emplace("child", Admit("child"));
  ASSERT_TRUE(workflow_->Watch(Target({"l1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Listening) << workflow_->CurrentStatus().error;
  EXPECT_EQ(relay_.attached_hop, "child");
  ASSERT_EQ(admissions_.size(), 2u);
  EXPECT_EQ(admissions_[1].second.path_stamp, std::vector<std::string>{"l1"});
}

TEST_F(BroadcastViewerWorkflowTest, HopWithoutAdmissionServiceIsAttachedDirectly) {
  ASSERT_TRUE(workflow_->Watch(Target({"plain-relay"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  EXPECT_EQ(relay_.attached_hop, "plain-relay");
}

TEST_F(BroadcastViewerWorkflowTest, TicketHopIsTriedAfterTheTipHops) {
  admission_.emplace("tip-hop", Refuse("full"));
  ticket_hop_ = "ticket-hop";
  ASSERT_TRUE(workflow_->Watch(Target({"tip-hop"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  EXPECT_EQ(relay_.attached_hop, "ticket-hop");
}

TEST_F(BroadcastViewerWorkflowTest, AttachFailureMovesToTheNextHop) {
  relay_.failing_hops["dead"] = true;
  ASSERT_TRUE(workflow_->Watch(Target({"dead", "alive"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  EXPECT_EQ(relay_.attach_hops, (std::vector<std::string>{"dead", "alive"}));
}

TEST_F(BroadcastViewerWorkflowTest, RefusedEverywhereFailsWithTheReason) {
  admission_.emplace("h1", Refuse("banned"));
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  const auto& status = workflow_->CurrentStatus();
  EXPECT_EQ(status.phase, Phase::Failed);
  EXPECT_NE(status.error.find("banned"), std::string::npos) << status.error;
  EXPECT_FALSE(engine_.IsActive());
}

TEST_F(BroadcastViewerWorkflowTest, TicketProblemsFailBeforeAnyRelayWork) {
  refuse_ticket_ = true;
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_NE(workflow_->CurrentStatus().error.find("not live"), std::string::npos);

  refuse_ticket_ = false;
  auto other = MlDsa::GenerateKeyPair();
  ASSERT_TRUE(other);
  verify_key_ = other->public_key;  // announce key does not match the ticket signer
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_NE(workflow_->CurrentStatus().error.find("ticket rejected"), std::string::npos)
      << workflow_->CurrentStatus().error;

  verify_key_.reset();
  publisher_reachable_ = false;
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  EXPECT_NE(workflow_->CurrentStatus().error.find("publisher unreachable"), std::string::npos);
  EXPECT_TRUE(relay_.attach_hops.empty());
}

TEST_F(BroadcastViewerWorkflowTest, RelayHeldByACallIsRefusedClearly) {
  relay_.attached = true;
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_EQ(workflow_->CurrentStatus().error, "media relay client in use by a call");
  EXPECT_EQ(relay_.detaches, 0) << "never detaches a session it does not own";
}

TEST_F(BroadcastViewerWorkflowTest, RelayLossReadmitsAndGivesUpAfterConsecutiveFailures) {
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  relay_.Lose();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Listening) << "loss is handled on UI";
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  EXPECT_EQ(workflow_->CurrentStatus().recoveries, 1);
  EXPECT_TRUE(engine_.IsActive()) << "playback session survives a re-admission";

  relay_.failing_hops["h1"] = true;
  relay_.Lose();
  Drain();
  const auto& status = workflow_->CurrentStatus();
  EXPECT_EQ(status.phase, Phase::Failed);
  EXPECT_NE(status.error.find("no relay admitted"), std::string::npos) << status.error;
}

// A call attaching takes the single relay client session: the viewer stops with a clear reason
// instead of listening to silence.
TEST_F(BroadcastViewerWorkflowTest, ACallTakingTheRelayStopsTheViewerClearly) {
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  const int detaches = relay_.detaches;
  relay_.Lose(MediaRelayClientLoss::Replaced);
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_EQ(workflow_->CurrentStatus().error, "media relay client in use by a call");
  EXPECT_EQ(relay_.detaches, detaches) << "never detaches the call's session";
  EXPECT_FALSE(engine_.IsActive());
}

TEST_F(BroadcastViewerWorkflowTest, StopDetachesStopsPlaybackAndDropsLateCompletions) {
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Listening);
  auto sink = relay_.sink;
  workflow_->Stop();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Idle);
  EXPECT_EQ(relay_.detaches, 1);
  EXPECT_FALSE(engine_.IsActive());
  EXPECT_TRUE(relay_.observers.empty());
  sink(SealedFrame(context_, 9));  // a frame already in flight
  EXPECT_EQ(engine_.HealthSnapshot().rx_audio_frames, 0u);
}

// Stop while AcceptAndAttach is on the wire: the late success must not leave the client attached.
TEST_F(BroadcastViewerWorkflowTest, LateAttachAfterStopIsDetached) {
  relay_.hold_attach = true;
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  ASSERT_EQ(workflow_->CurrentStatus().phase, Phase::Attaching);
  workflow_->Stop();
  relay_.held();
  Drain();
  EXPECT_FALSE(relay_.attached);
  EXPECT_EQ(relay_.detaches, 1);
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Idle);
}

TEST_F(BroadcastViewerWorkflowTest, PaidRelayQuotesAreDeclined) {
  relay_.rate = 0.5;
  ASSERT_TRUE(workflow_->Watch(Target({"h1"})));
  Drain();
  EXPECT_EQ(workflow_->CurrentStatus().phase, Phase::Failed);
  EXPECT_NE(workflow_->CurrentStatus().error.find("paid"), std::string::npos) << workflow_->CurrentStatus().error;
}

// The feature root: a Live tip in, a listening viewer out — no call objects anywhere.
TEST_F(BroadcastViewerWorkflowTest, HubWatchesALiveTipAndStopsOnDestruction) {
  admission_.emplace("hop", Admit("hop"));
  PeerAnnounceTip tip;
  tip.peer_id = kPublisher;
  tip.program_id = kProgram;
  tip.join_handle = kJoin;
  tip.state = PeerAnnounceState::Live;
  tip.hop_peer_id = "hop";
  {
    BroadcastHub hub(Ports(), devices_);
    int changes = 0;
    hub.SetOnChanged([&changes]() { ++changes; });
    ASSERT_TRUE(hub.WatchLive(tip));
    Drain();
    EXPECT_TRUE(hub.IsWatching());
    EXPECT_EQ(hub.Viewer().phase, Phase::Listening);
    EXPECT_GT(changes, 0);
    for (int i = 0; i < 400 && devices_.Holders(MediaDeviceKind::Speaker).empty(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));  // leased on the engine's device thread
    }
    EXPECT_EQ(devices_.Holders(MediaDeviceKind::Speaker), std::vector<std::string>{kJoin});
    tip.state = PeerAnnounceState::Ended;
    EXPECT_FALSE(hub.WatchLive(tip)) << "an ended program is not watchable";
    EXPECT_TRUE(hub.IsWatching()) << "a refused tip leaves the current watch alone";
  }
  EXPECT_FALSE(relay_.attached);
  EXPECT_TRUE(devices_.Holders(MediaDeviceKind::Speaker).empty());
}

TEST(BroadcastWatchTargetTest, OnlyLiveProgramTipsAreWatchable) {
  PeerAnnounceTip tip;
  tip.peer_id = kPublisher;
  tip.program_id = kProgram;
  tip.join_handle = kJoin;
  tip.state = PeerAnnounceState::Live;
  tip.hop_peer_id = "hop";
  tip.l1_hop_peer_ids = {"hop", "l1b"};
  auto target = BroadcastWatchTargetFromTip(tip);
  ASSERT_TRUE(target);
  EXPECT_EQ(target->hops, (std::vector<std::string>{"hop", "l1b"}));
  tip.state = PeerAnnounceState::Scheduled;
  EXPECT_FALSE(BroadcastWatchTargetFromTip(tip));
  tip.state = PeerAnnounceState::Live;
  tip.kind = kPeerAnnounceKindLiveChat;
  EXPECT_FALSE(BroadcastWatchTargetFromTip(tip));
}

} // namespace
} // namespace pbr
