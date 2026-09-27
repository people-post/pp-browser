// Live broadcast viewer end to end on the in-process mesh (media-client-layers l4c):
// publisher B serves the real broadcast RPC (ticket) and publishes sealed frames to hop R;
// viewer A runs BroadcastViewerWorkflow with the real RPC client + media_relay client.
// (The triple harness accepts inbound on R and B only — A is the pure client.)
// R is a plain media_relay hop without an admission service — the viewer attaches directly (L013).

#include "feature/broadcast/AmpBroadcastRpcClient.h"
#include "feature/broadcast/BroadcastViewerWorkflow.h"
#include "feature/broadcast/BroadcasterWorkflow.h"
#include "feature/conversations/AmpBroadcastTransport.h"

#include "domain/media/CallMediaEngine.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/VideoCodecUnavailable.h"
#include "domain/mesh/l4/media_relay/AmpMediaRelayClient.h"
#include "domain/mesh/l4/media_relay/AmpMediaRelayCoordinator.h"
#include "domain/mesh/l4/media_relay/MediaRelayFrameCrypto.h"
#include "domain/mesh/tests/support/mesh_triple_harness.h"
#include "domain/messaging/BroadcastMedia.h"
#include "foundation/crypto/MlDsa.h"

#include <gtest/gtest.h>
#include <opus.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr const char* kProgram = "show-compose";
constexpr const char* kJoin = "live:show-compose";

class AlwaysDialable final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string&, const std::string&) override { return {}; }
  bool IsDialable(const std::string&) const override { return true; }
  std::optional<std::string> PreferredMultiaddr(const std::string&) const override { return std::nullopt; }
  void ClearDialBackoff(const std::string&) override {}
  void AbortInflightDial(const std::string&) override {}
  void ClearPeerCircuitHop(const std::string&) override {}
};

std::vector<uint8_t> OpusFrame() {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  std::vector<int16_t> pcm(960, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 960, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  return {out.begin(), out.begin() + std::max(n, 0)};
}

class BroadcastViewerComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    auto created = test::AmpMeshTripleHarness::Create();
    ASSERT_TRUE(created) << created.error().message;
    h_ = std::move(*created);
    publisher_ = h_->peer_id_b;
    hop_ = h_->peer_id_r;
    viewer_ = h_->peer_id_a;
    ASSERT_TRUE(h_->mgr_b().RegisterEndpoint(hop_, h_->ma_r));
    ASSERT_TRUE(h_->mgr_a().RegisterEndpoint(hop_, h_->ma_r));
    ASSERT_TRUE(h_->mgr_a().RegisterEndpoint(publisher_, h_->ma_b));

    links_a_ = NewAmpChatPeerLinks(*h_->runtime_a);
    links_b_ = NewAmpChatPeerLinks(*h_->runtime_b);
    // RPC channels go through the chat-link adapter's own endpoint book.
    ASSERT_TRUE(links_a_->RegisterEndpoint(publisher_, h_->ma_b));
    ASSERT_TRUE(links_a_->RegisterEndpoint(hop_, h_->ma_r));
    auto pump = [this]() { h_->PumpAll(); };

    auto keys = MlDsa::GenerateKeyPair();
    ASSERT_TRUE(keys);
    publisher_keys_ = *keys;
    media_key_.assign(32, 0x33);

    // Publisher side: ticket server (l5 wires this from the live program; here by hand).
    server_ = std::make_unique<AmpBroadcastTransport>(*links_b_, pump, AmpBroadcastTransport::WorkerPost{},
                                                      PostIo(*h_->runtime_b), PostAfter(*h_->runtime_b));
    server_->SetPublisherSecretResolver([this]() -> std::optional<ByteVector> { return publisher_keys_.secret_key; });
    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = publisher_;
    live.media_key_bytes = media_key_;
    live.media_epoch = 1;
    live.hop_peer_id = hop_;
    server_->PutLiveProgramKey(kProgram, kJoin, live);
    server_->Start();

    hop_relay_ = std::make_unique<AmpMediaRelayCoordinator>(*h_->runtime_r);
    publisher_relay_ = std::make_unique<AmpMediaRelayCoordinator>(*h_->runtime_b);
    viewer_relay_coord_ = std::make_unique<AmpMediaRelayCoordinator>(*h_->runtime_a);
    hop_relay_->Start();
    publisher_relay_->Start();
    viewer_relay_coord_->Start();
    viewer_relay_ = std::make_unique<AmpMediaRelayClient>(*viewer_relay_coord_, pump, viewer_,
                                                          PostIo(*h_->runtime_a), PostAfter(*h_->runtime_a));

    rpc_ = std::make_unique<AmpBroadcastRpcClient>(*links_a_, pump, PostIo(*h_->runtime_a), PostAfter(*h_->runtime_a));
    engine_ = std::make_unique<CallMediaEngine>(devices_);
    engine_->SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test"); });
    workflow_ = std::make_unique<BroadcastViewerWorkflow>(ViewerPorts());
  }

  void TearDown() override {
    workflow_.reset();
    if (engine_) {
      engine_->Stop();
    }
    rpc_.reset();
    viewer_relay_.reset();
    for (auto* coord : {viewer_relay_coord_.get(), publisher_relay_.get(), hop_relay_.get()}) {
      if (coord) {
        coord->Stop();
      }
    }
    viewer_relay_coord_.reset();
    publisher_relay_.reset();
    hop_relay_.reset();
    if (server_) {
      server_->Stop();
    }
    server_.reset();
    hop_admission_.reset();
    hop_links_.reset();
    links_a_.reset();
    links_b_.reset();
    h_.reset();
  }

  static std::function<void(std::function<void()>)> PostIo(pp::amp::MeshRuntime& rt) {
    return [&rt](std::function<void()> task) { rt.PostToIo(std::move(task)); };
  }
  static std::function<void(std::chrono::milliseconds, std::function<void()>)> PostAfter(pp::amp::MeshRuntime& rt) {
    return [&rt](std::chrono::milliseconds delay, std::function<void()> task) { rt.PostAfter(delay, std::move(task)); };
  }

  BroadcastViewerPorts ViewerPorts() {
    BroadcastViewerPorts p;
    p.local_peer_id = [this]() { return viewer_; };
    p.publisher_key = [this](const std::string& peer) -> std::optional<ByteVector> {
      return peer == publisher_ ? std::optional<ByteVector>(publisher_keys_.public_key) : std::nullopt;
    };
    p.request_ticket = [this](const std::string& publisher, const BroadcastTicketRequest& req,
                              std::function<void(Roe<BroadcastTicketResponse>)> done) {
      rpc_->RequestTicketAsync(publisher, req, std::move(done));
    };
    p.request_admission = [this](const std::string& hop, const BroadcastViewerAttachRequest& req,
                                 std::function<void(Roe<BroadcastViewerAttachResult>)> done) {
      rpc_->RequestViewerAttachAsync(hop, req, std::move(done));
    };
    p.relay.relay = viewer_relay_.get();
    p.relay.dial = &dial_;
    p.engine = engine_.get();
    p.post_ui = [this](std::function<void()> task) { ui_.push_back(std::move(task)); };
    p.post_ui_after = [this](std::chrono::milliseconds, std::function<void()> task) { ui_.push_back(std::move(task)); };
    p.now_ms = []() {
      return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
    return p;
  }

  /** Pump the mesh and run posted UI tasks until `done` (bounded). */
  bool RunUntil(const std::function<bool()>& done, int rounds = 3000) {
    for (int i = 0; i < rounds && !done(); ++i) {
      h_->PumpAll();
      while (!ui_.empty()) {
        auto task = std::move(ui_.front());
        ui_.pop_front();
        task();
      }
    }
    return done();
  }

  void PublisherAttach() {
    std::atomic<bool> quoted{false};
    Roe<MediaRelayQuote> quote = Error("pending");
    MediaRelayQuoteRequest req;
    req.session_id = kJoin;
    ASSERT_TRUE(publisher_relay_->StartQuote(hop_, req, [&](Roe<MediaRelayQuote> q) {
      quote = std::move(q);
      quoted = true;
    }));
    ASSERT_TRUE(RunUntil([&] { return quoted.load(); }));
    ASSERT_TRUE(quote && quote->ok) << (quote ? quote->error : quote.error().message);
    std::atomic<bool> attached{false};
    Roe<MediaRelayAttachResult> result = Error("pending");
    ASSERT_TRUE(publisher_relay_->StartAttach(hop_, quote->quote_id, kJoin, kJoin, {}, [&](Roe<MediaRelayAttachResult> r) {
      result = std::move(r);
      attached = true;
    }));
    ASSERT_TRUE(RunUntil([&] { return attached.load(); }));
    ASSERT_TRUE(result && result->ok);
  }

  void PublishFrame(uint32_t seq) {
    const uint32_t stream = BroadcastPublisherStreamId(publisher_);
    auto body = SealMediaRelayFrame(media_key_, BroadcastMediaFrameContext(kProgram, kJoin), 1, stream, seq, 0, 0, opus_);
    ASSERT_TRUE(body);
    MediaDataFrame frame;
    frame.stream_id = stream;
    frame.channel_id = 0;
    frame.seq = seq;
    frame.payload = std::move(*body);
    ASSERT_TRUE(publisher_relay_->SendFrame(frame));
  }

  std::unique_ptr<test::AmpMeshTripleHarness> h_;
  std::string publisher_, hop_, viewer_;
  std::unique_ptr<IChatPeerLinks> links_a_;
  std::unique_ptr<IChatPeerLinks> links_b_;
  MlDsaKeyPair publisher_keys_;
  ByteVector media_key_;
  const std::vector<uint8_t> opus_ = OpusFrame();
  std::unique_ptr<AmpBroadcastTransport> server_;
  std::unique_ptr<IChatPeerLinks> hop_links_;
  std::unique_ptr<AmpBroadcastTransport> hop_admission_;
  std::unique_ptr<AmpMediaRelayCoordinator> hop_relay_;
  std::unique_ptr<AmpMediaRelayCoordinator> publisher_relay_;
  std::unique_ptr<AmpMediaRelayCoordinator> viewer_relay_coord_;
  std::unique_ptr<AmpMediaRelayClient> viewer_relay_;
  std::unique_ptr<AmpBroadcastRpcClient> rpc_;
  AlwaysDialable dial_;
  MediaDeviceArbiter devices_{CreateNullMediaDeviceBackend()};
  std::unique_ptr<CallMediaEngine> engine_;
  std::unique_ptr<BroadcastViewerWorkflow> workflow_;
  std::deque<std::function<void()>> ui_;
};

TEST_F(BroadcastViewerComposeTest, ViewerListensToThePublisherThroughAPlainRelayHop) {
  PublisherAttach();
  ASSERT_TRUE(workflow_->Watch(BroadcastWatchTarget{publisher_, kProgram, kJoin, {hop_}}));
  ASSERT_TRUE(RunUntil([&] {
    const auto phase = workflow_->CurrentStatus().phase;
    return phase == BroadcastViewerWorkflow::Phase::Listening || phase == BroadcastViewerWorkflow::Phase::Failed;
  })) << "phase=" << BroadcastViewerWorkflow::PhaseName(workflow_->CurrentStatus().phase);
  ASSERT_EQ(workflow_->CurrentStatus().phase, BroadcastViewerWorkflow::Phase::Listening)
      << workflow_->CurrentStatus().error;
  EXPECT_EQ(workflow_->CurrentStatus().hop, hop_);
  EXPECT_EQ(engine_->ActiveSpec(), CallMediaEngine::SessionSpec::PlaybackOnly());

  // Let the viewer's subscribe reach the hop before the publisher sends.
  for (int i = 0; i < 40; ++i) {
    h_->PumpAll();
  }
  uint32_t seq = 1;
  ASSERT_TRUE(RunUntil([&] {
    PublishFrame(seq++);
    return engine_->HealthSnapshot().rx_audio_frames >= 3;
  })) << "rx=" << engine_->HealthSnapshot().rx_audio_frames;

  workflow_->Stop();
  EXPECT_FALSE(viewer_relay_->IsAttached());
  EXPECT_FALSE(engine_->IsActive());
}

// The publisher only mints for a program it is live on: a viewer asking for another program fails
// at the ticket step, before any relay work.
// A hop that serves admission (a B1 ladder relay): the viewer is admitted, then attaches there.
TEST_F(BroadcastViewerComposeTest, AdmittingHopAdmitsThenTheViewerListens) {
  hop_links_ = NewAmpChatPeerLinks(*h_->runtime_r);
  hop_admission_ = std::make_unique<AmpBroadcastTransport>(*hop_links_, [this]() { h_->PumpAll(); },
                                                           AmpBroadcastTransport::WorkerPost{}, PostIo(*h_->runtime_r),
                                                           PostAfter(*h_->runtime_r));
  hop_admission_->SetPublisherKeyResolver([this](const std::string& peer) -> std::optional<ByteVector> {
    return peer == publisher_ ? std::optional<ByteVector>(publisher_keys_.public_key) : std::nullopt;
  });
  hop_admission_->SetHopAttachResolver([this](const std::string&, const std::string&) {
    AmpBroadcastTransport::HopAttachContext ctx;
    ctx.free_viewer_slots = 1;
    ctx.self_peer_id = hop_;
    return ctx;
  });
  hop_admission_->Start();
  PublisherAttach();
  ASSERT_TRUE(workflow_->Watch(BroadcastWatchTarget{publisher_, kProgram, kJoin, {hop_}}));
  ASSERT_TRUE(RunUntil([&] { return workflow_->CurrentStatus().phase == BroadcastViewerWorkflow::Phase::Listening; },
                       600))
      << "phase=" << BroadcastViewerWorkflow::PhaseName(workflow_->CurrentStatus().phase) << " "
      << workflow_->CurrentStatus().error << " (admitted without waiting out the admission timeout)";
  hop_admission_->Stop();
}

// l5: nothing hand-published — B's BroadcasterWorkflow goes live through R (real key to its ticket
// server, capture engine sealing silence frames), A watches from the tip B announced.
TEST_F(BroadcastViewerComposeTest, BroadcasterToRelayToViewerEndToEnd) {
  std::vector<BroadcastTipDraft> tips;
  auto publisher_client = std::make_unique<AmpMediaRelayClient>(*publisher_relay_, [this]() { h_->PumpAll(); },
                                                                publisher_, PostIo(*h_->runtime_b),
                                                                PostAfter(*h_->runtime_b));
  CallMediaEngine capture(devices_);
  capture.SetVideoCodecFactoryForTest([]() { return MakeUnavailableVideoCodec("test"); });
  BroadcasterPorts bp;
  bp.local_peer_id = [this]() { return publisher_; };
  bp.new_media_key = []() { return NewBroadcastMediaKey(); };
  bp.new_join_handle = [](const std::string& program) { return NewBroadcastJoinHandle(program); };
  bp.put_program_key = [this](const std::string& program, const std::string& join, BroadcastProgramKey key) {
    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = key.publisher_peer_id;
    live.media_key_bytes = key.media_key;
    live.media_epoch = key.media_epoch;
    live.hop_peer_id = key.hop_peer_id;
    server_->PutLiveProgramKey(program, join, live);
  };
  bp.clear_program_key = [this](const std::string& program, const std::string& join) {
    server_->ClearLiveProgramKey(program, join);
  };
  bp.announce = [&tips](const BroadcastTipDraft& draft) -> Roe<void> {
    tips.push_back(draft);
    return {};
  };
  bp.relay.relay = publisher_client.get();
  bp.relay.dial = &dial_;
  bp.engine = &capture;
  bp.post_ui = [this](std::function<void()> task) { ui_.push_back(std::move(task)); };
  auto broadcaster = std::make_unique<BroadcasterWorkflow>(bp);

  ASSERT_TRUE(broadcaster->GoLive({"topic", "show-e2e", {hop_}}));
  ASSERT_TRUE(RunUntil([&] { return broadcaster->CurrentStatus().phase == BroadcasterWorkflow::Phase::Live; }))
      << BroadcasterWorkflow::PhaseName(broadcaster->CurrentStatus().phase) << " " << broadcaster->CurrentStatus().error;
  ASSERT_EQ(tips.size(), 1u);
  const BroadcastTipDraft live = tips.front();

  PeerAnnounceTip tip;
  tip.peer_id = publisher_;
  tip.topic_id = live.topic_id;
  tip.program_id = live.program_id;
  tip.join_handle = live.join_handle;
  tip.state = PeerAnnounceState::Live;
  tip.hop_peer_id = live.hop_peer_id;
  auto target = BroadcastWatchTargetFromTip(tip);
  ASSERT_TRUE(target);
  ASSERT_TRUE(workflow_->Watch(*target));
  ASSERT_TRUE(RunUntil([&] {
    const auto phase = workflow_->CurrentStatus().phase;
    return phase == BroadcastViewerWorkflow::Phase::Listening || phase == BroadcastViewerWorkflow::Phase::Failed;
  }));
  ASSERT_EQ(workflow_->CurrentStatus().phase, BroadcastViewerWorkflow::Phase::Listening)
      << workflow_->CurrentStatus().error;
  // Capture runs on real time (20 ms frames): pace the pump instead of spinning virtual time.
  ASSERT_TRUE(RunUntil(
      [&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return engine_->HealthSnapshot().rx_audio_frames >= 5;
      },
      5000))
      << "sent=" << broadcaster->CurrentStatus().frames_sent << " rx=" << engine_->HealthSnapshot().rx_audio_frames;

  broadcaster->End();
  ASSERT_EQ(tips.back().state, PeerAnnounceState::Ended);
  // The show's key is gone: a late viewer is refused a ticket.
  workflow_->Stop();
  ASSERT_TRUE(workflow_->Watch(*target));
  ASSERT_TRUE(RunUntil([&] { return workflow_->CurrentStatus().phase == BroadcastViewerWorkflow::Phase::Failed; }));
  EXPECT_NE(workflow_->CurrentStatus().error.find("ticket"), std::string::npos) << workflow_->CurrentStatus().error;
  broadcaster.reset();
  capture.Stop();
  publisher_client.reset();
}

TEST_F(BroadcastViewerComposeTest, UnknownProgramFailsAtTheTicket) {
  ASSERT_TRUE(workflow_->Watch(BroadcastWatchTarget{publisher_, "other-show", "live:other", {hop_}}));
  ASSERT_TRUE(RunUntil([&] { return workflow_->CurrentStatus().phase == BroadcastViewerWorkflow::Phase::Failed; }));
  EXPECT_NE(workflow_->CurrentStatus().error.find("ticket"), std::string::npos) << workflow_->CurrentStatus().error;
  EXPECT_FALSE(viewer_relay_->IsAttached());
}

} // namespace
} // namespace pbr
