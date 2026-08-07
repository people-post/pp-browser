#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include "base/people/RelayScope.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <process.h>
static int ProcessId() { return _getpid(); }
#else
#include <unistd.h>
static int ProcessId() { return static_cast<int>(getpid()); }
#endif

namespace pbr {
namespace {

class MediaRelayServiceTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<int> port{43000 + (ProcessId() % 2000) * 10};
    hop_port_ = port.fetch_add(1);
    a_port_ = port.fetch_add(1);
    b_port_ = port.fetch_add(1);

    PeerSessionConfig config;
    config.dial_timeout = std::chrono::milliseconds(3000);
    config.dial_failure_backoff = std::chrono::milliseconds(100);

    Libp2pHostConfig hop_cfg;
    hop_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_);
    ASSERT_TRUE(hop_host_.Start(hop_cfg));
    hop_sessions_ = std::make_unique<PeerSessionManager>(hop_host_, config);
    hop_relay_ = std::make_unique<MediaRelayService>(hop_host_, *hop_sessions_);
    hop_relay_->Start();

    Libp2pHostConfig a_cfg;
    a_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(a_port_);
    ASSERT_TRUE(a_host_.Start(a_cfg));
    a_sessions_ = std::make_unique<PeerSessionManager>(a_host_, config);
    a_relay_ = std::make_unique<MediaRelayService>(a_host_, *a_sessions_);

    Libp2pHostConfig b_cfg;
    b_cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/" + std::to_string(b_port_);
    ASSERT_TRUE(b_host_.Start(b_cfg));
    b_sessions_ = std::make_unique<PeerSessionManager>(b_host_, config);
    b_relay_ = std::make_unique<MediaRelayService>(b_host_, *b_sessions_);
  }

  void TearDown() override {
    a_relay_.reset();
    b_relay_.reset();
    hop_relay_.reset();
    a_sessions_.reset();
    b_sessions_.reset();
    hop_sessions_.reset();
    a_host_.Stop();
    b_host_.Stop();
    hop_host_.Stop();
  }

  int hop_port_ = 0;
  int a_port_ = 0;
  int b_port_ = 0;
  Libp2pHost hop_host_;
  Libp2pHost a_host_;
  Libp2pHost b_host_;
  std::unique_ptr<PeerSessionManager> hop_sessions_;
  std::unique_ptr<PeerSessionManager> a_sessions_;
  std::unique_ptr<PeerSessionManager> b_sessions_;
  std::unique_ptr<MediaRelayService> hop_relay_;
  std::unique_ptr<MediaRelayService> a_relay_;
  std::unique_ptr<MediaRelayService> b_relay_;
};

TEST_F(MediaRelayServiceTest, QuoteAcceptAttachFanout) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));
  ASSERT_TRUE(b_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-test-1";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;
  EXPECT_EQ(qa->pricing_mode, "volunteer");

  auto qb = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb) << qb.error().message;
  ASSERT_TRUE(qb->ok) << qb->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;

  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;

  auto attach_b = b_relay_->AcceptAndAttach(
      "hop", qb->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_b) << attach_b.error().message;
  ASSERT_TRUE(attach_b->ok) << attach_b->error;

  a_relay_->StartClientFrameReader();
  b_relay_->StartClientFrameReader();

  ASSERT_TRUE(b_relay_->Subscribe(1, 0));
  // Give subscribe a moment to land on hop.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::LatestLossy;
  sent.seq = 1;
  sent.payload = {'h', 'i'};
  ASSERT_TRUE(a_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }));
  }
  EXPECT_EQ(received.stream_id, 1u);
  EXPECT_EQ(received.payload, sent.payload);

  a_relay_->Detach();
  b_relay_->Detach();
}

TEST_F(MediaRelayServiceTest, CallScopedAdmissionAllowsStrangerAfterSponsor) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  auto a_id = a_host_.LocalPeerIdBase58();
  ASSERT_TRUE(a_id);
  auto b_id = b_host_.LocalPeerIdBase58();
  ASSERT_TRUE(b_id);

  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));
  ASSERT_TRUE(b_sessions_->RegisterEndpoint("hop", hop_ma));

  MediaRelayAdmissionPolicy policy;
  policy.prefer_contacts_only = true;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial; // no Public
  policy.contact_peer_ids = {*a_id};
  hop_relay_->SetAdmissionPolicy(std::move(policy));

  const std::string call_id = "call-scoped-1";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  // Stranger-first: B alone must be refused.
  auto qb_early = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb_early) << qb_early.error().message;
  EXPECT_FALSE(qb_early->ok);
  EXPECT_NE(qb_early->error.find("stranger refused"), std::string::npos);

  // Sponsor A opens the session.
  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;
  auto attach_a = a_relay_->AcceptAndAttach("hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;

  // Stranger B may join the same call_id after sponsor attached.
  auto qb = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb) << qb.error().message;
  ASSERT_TRUE(qb->ok) << qb->error;
  auto attach_b = b_relay_->AcceptAndAttach("hop", qb->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_b) << attach_b.error().message;
  ASSERT_TRUE(attach_b->ok) << attach_b->error;

  (void)b_id;
  a_relay_->Detach();
  b_relay_->Detach();
}

TEST_F(MediaRelayServiceTest, PreferLocalHopFanoutToGuest) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-local-hop-fanout";
  auto local = hop_relay_->AttachAsLocalHop(call_id, [](MediaDataFrame) {});
  ASSERT_TRUE(local) << local.error().message;
  ASSERT_TRUE(local->ok) << local->error;
  EXPECT_TRUE(hop_relay_->IsLocalHopAttached());

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;
  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;
  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;
  a_relay_->StartClientFrameReader();
  ASSERT_TRUE(a_relay_->Subscribe(42, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MediaDataFrame sent;
  sent.stream_id = 42;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::ReliableOrdered;
  sent.seq = 7;
  sent.payload = {'p', 'c', 'm'};
  ASSERT_TRUE(hop_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }))
        << "guest must receive PreferLocal hop frames";
  }
  EXPECT_EQ(received.stream_id, 42u);
  EXPECT_EQ(received.seq, 7u);
  EXPECT_EQ(received.payload, sent.payload);

  hop_relay_->Detach();
  a_relay_->Detach();
}

// Dogfood: Samsung heard Moto via PreferLocal hop, but Linux (local hop owner) did not.
// Guest uplink must Fanout into local_on_frame when the hop subscribed to that stream.
TEST_F(MediaRelayServiceTest, PreferLocalHopFanoutGuestToLocal) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-local-hop-guest-to-local";
  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;
  auto local = hop_relay_->AttachAsLocalHop(call_id, [&](MediaDataFrame frame) {
    std::lock_guard<std::mutex> lock(mu);
    received = std::move(frame);
    got = true;
    cv.notify_one();
  });
  ASSERT_TRUE(local) << local.error().message;
  ASSERT_TRUE(local->ok) << local->error;
  ASSERT_TRUE(hop_relay_->Subscribe(3272724854u, 0));

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;
  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;

  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;
  a_relay_->StartClientFrameReader();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MediaDataFrame sent;
  sent.stream_id = 3272724854u;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::ReliableOrdered;
  sent.seq = 335;
  sent.payload = {'m', 'o', 't', 'o'};
  ASSERT_TRUE(a_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }))
        << "PreferLocal hop owner must receive guest uplink (Moto→Linux)";
  }
  EXPECT_EQ(received.stream_id, 3272724854u);
  EXPECT_EQ(received.seq, 335u);
  EXPECT_EQ(received.payload, sent.payload);

  hop_relay_->Detach();
  a_relay_->Detach();
}

TEST_F(MediaRelayServiceTest, CallScopedAdmissionLocalHopUnlocksStranger) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  auto b_id = b_host_.LocalPeerIdBase58();
  ASSERT_TRUE(b_id);

  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(b_sessions_->RegisterEndpoint("hop", hop_ma));

  MediaRelayAdmissionPolicy policy;
  policy.prefer_contacts_only = true;
  policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
  // Empty contacts would admit everyone; use a dummy contact so B is a stranger.
  policy.contact_peer_ids = {"12D3KooWNotB"};
  hop_relay_->SetAdmissionPolicy(std::move(policy));

  const std::string call_id = "call-local-sponsor";
  auto local = hop_relay_->AttachAsLocalHop(call_id, [](MediaDataFrame) {});
  ASSERT_TRUE(local) << local.error().message;
  ASSERT_TRUE(local->ok) << local->error;

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;
  auto qb = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb) << qb.error().message;
  ASSERT_TRUE(qb->ok) << qb->error;
  auto attach_b = b_relay_->AcceptAndAttach("hop", qb->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_b) << attach_b.error().message;
  ASSERT_TRUE(attach_b->ok) << attach_b->error;

  (void)b_id;
  hop_relay_->Detach();
  b_relay_->Detach();
}

TEST_F(MediaRelayServiceTest, AsyncDataPlaneQuoteAcceptAttachFanout) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));
  ASSERT_TRUE(b_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-async-1";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;

  auto qb = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb) << qb.error().message;
  ASSERT_TRUE(qb->ok) << qb->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;

  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;

  auto attach_b = b_relay_->AcceptAndAttach(
      "hop", qb->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_b) << attach_b.error().message;
  ASSERT_TRUE(attach_b->ok) << attach_b->error;

  a_relay_->StartClientFrameReader();
  b_relay_->StartClientFrameReader();

  ASSERT_TRUE(b_relay_->Subscribe(1, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));

  MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::LatestLossy;
  sent.seq = 1;
  sent.payload = {'a', 's', 'y', 'n', 'c'};
  ASSERT_TRUE(a_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }));
  }
  EXPECT_EQ(received.stream_id, 1u);
  EXPECT_EQ(received.payload, sent.payload);

  a_relay_->Detach();
  b_relay_->Detach();
}

TEST_F(MediaRelayServiceTest, AsyncDataPlaneDetachUnblocksAttachedClient) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-async-detach";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 1;

  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;

  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;
  EXPECT_TRUE(a_relay_->IsAttached());
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Attached);

  a_relay_->StartClientFrameReader();
  a_relay_->Detach();
  EXPECT_FALSE(a_relay_->IsAttached());
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Idle);
}

TEST_F(MediaRelayServiceTest, DetachUnblocksAcceptAndAttachWait) {
  // Dial a documentation blackhole address so AcceptAndAttach stays in Dialing until Detach.
  auto peer_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(peer_id);
  ASSERT_TRUE(a_sessions_->RegisterEndpoint(
      "blackhole", "/ip4/192.0.2.1/tcp/4001/p2p/" + *peer_id));

  std::atomic<bool> done{false};
  Roe<MediaRelayAttachResult> result = Error("not run");
  std::thread th([&] {
    result = a_relay_->AcceptAndAttach("blackhole", "q-missing", "call-detach-wait",
                                       "call-detach-wait", [](MediaDataFrame) {}, 15000);
    done.store(true, std::memory_order_release);
  });

  for (int i = 0; i < 100 && a_relay_->ClientPhase() == MediaRelayClientPhase::Idle; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Dialing);
  ASSERT_FALSE(done.load(std::memory_order_acquire));

  a_relay_->Detach();
  th.join();

  ASSERT_TRUE(done.load(std::memory_order_acquire));
  EXPECT_FALSE(result);
  EXPECT_EQ(result.error().message, "media-relay attach aborted");
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Idle);
  EXPECT_FALSE(a_relay_->IsAttached());
}

// PreferLocal SoftMigrate-style: guest Detach → reattach while local hop owner stays; fan-out resumes.
TEST_F(MediaRelayServiceTest, PreferLocalGuestDetachThenReattachFanout) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-prefer-local-reattach";
  auto local = hop_relay_->AttachAsLocalHop(call_id, [](MediaDataFrame) {});
  ASSERT_TRUE(local) << local.error().message;
  ASSERT_TRUE(local->ok) << local->error;
  EXPECT_TRUE(hop_relay_->IsLocalHopAttached());

  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;
  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;
  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Attached);

  a_relay_->StartClientFrameReader();
  ASSERT_TRUE(a_relay_->Subscribe(42, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  a_relay_->Detach();
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Idle);
  EXPECT_TRUE(hop_relay_->IsLocalHopAttached()) << "PreferLocal owner must survive guest Detach";

  got = false;
  auto qa2 = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa2) << qa2.error().message;
  ASSERT_TRUE(qa2->ok) << qa2->error;
  auto attach_a2 = a_relay_->AcceptAndAttach(
      "hop", qa2->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_a2) << attach_a2.error().message;
  ASSERT_TRUE(attach_a2->ok) << attach_a2->error;
  EXPECT_EQ(a_relay_->ClientPhase(), MediaRelayClientPhase::Attached);

  a_relay_->StartClientFrameReader();
  ASSERT_TRUE(a_relay_->Subscribe(42, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MediaDataFrame sent;
  sent.stream_id = 42;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::ReliableOrdered;
  sent.seq = 9;
  sent.payload = {'p', 'l'};
  ASSERT_TRUE(hop_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }))
        << "guest must receive PreferLocal frames after SoftMigrate-style reattach";
  }
  EXPECT_EQ(received.payload, sent.payload);

  hop_relay_->Detach();
  a_relay_->Detach();
}

// SoftMigrate-style: guest Detach → Idle, then re-quote/attach while peer A stays; fan-out resumes.
TEST_F(MediaRelayServiceTest, GuestDetachThenReattachFanout) {
  auto hop_id = hop_host_.LocalPeerIdBase58();
  ASSERT_TRUE(hop_id);
  const std::string hop_ma = "/ip4/127.0.0.1/tcp/" + std::to_string(hop_port_) + "/p2p/" + *hop_id;
  ASSERT_TRUE(a_sessions_->RegisterEndpoint("hop", hop_ma));
  ASSERT_TRUE(b_sessions_->RegisterEndpoint("hop", hop_ma));

  const std::string call_id = "call-guest-reattach";
  MediaRelayQuoteRequest qreq;
  qreq.call_id = call_id;
  qreq.participants = 2;

  auto qa = a_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qa) << qa.error().message;
  ASSERT_TRUE(qa->ok) << qa->error;
  auto qb = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb) << qb.error().message;
  ASSERT_TRUE(qb->ok) << qb->error;

  auto attach_a = a_relay_->AcceptAndAttach(
      "hop", qa->quote_id, call_id, call_id, [](MediaDataFrame) {}, 5000);
  ASSERT_TRUE(attach_a) << attach_a.error().message;
  ASSERT_TRUE(attach_a->ok) << attach_a->error;

  std::mutex mu;
  std::condition_variable cv;
  bool got = false;
  MediaDataFrame received;
  auto attach_b = b_relay_->AcceptAndAttach(
      "hop", qb->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_b) << attach_b.error().message;
  ASSERT_TRUE(attach_b->ok) << attach_b->error;
  EXPECT_EQ(b_relay_->ClientPhase(), MediaRelayClientPhase::Attached);

  a_relay_->StartClientFrameReader();
  b_relay_->StartClientFrameReader();
  ASSERT_TRUE(b_relay_->Subscribe(1, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // SoftMigrate / Leave on guest: Detach then re-AcceptAndAttach on same call_id.
  b_relay_->Detach();
  EXPECT_EQ(b_relay_->ClientPhase(), MediaRelayClientPhase::Idle);
  EXPECT_FALSE(b_relay_->IsAttached());
  EXPECT_TRUE(a_relay_->IsAttached());

  got = false;
  auto qb2 = b_relay_->RequestQuote("hop", qreq, 5000);
  ASSERT_TRUE(qb2) << qb2.error().message;
  ASSERT_TRUE(qb2->ok) << qb2->error;
  auto attach_b2 = b_relay_->AcceptAndAttach(
      "hop", qb2->quote_id, call_id, call_id,
      [&](MediaDataFrame frame) {
        std::lock_guard<std::mutex> lock(mu);
        received = std::move(frame);
        got = true;
        cv.notify_one();
      },
      5000);
  ASSERT_TRUE(attach_b2) << attach_b2.error().message;
  ASSERT_TRUE(attach_b2->ok) << attach_b2->error;
  EXPECT_EQ(b_relay_->ClientPhase(), MediaRelayClientPhase::Attached);

  b_relay_->StartClientFrameReader();
  ASSERT_TRUE(b_relay_->Subscribe(1, 0));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  MediaDataFrame sent;
  sent.stream_id = 1;
  sent.channel_id = 0;
  sent.channel_type = MediaChannelType::LatestLossy;
  sent.seq = 2;
  sent.payload = {'r', 'e'};
  ASSERT_TRUE(a_relay_->SendFrame(sent));

  {
    std::unique_lock<std::mutex> lock(mu);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return got; }))
        << "guest must receive frames after SoftMigrate-style reattach";
  }
  EXPECT_EQ(received.payload, sent.payload);

  a_relay_->Detach();
  b_relay_->Detach();
}

} // namespace
} // namespace pbr
