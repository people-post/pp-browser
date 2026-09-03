#include "feature/conversations/calls/CallTopologyController.h"
#include "feature/conversations/calls/CallTopologyRelayDeps.h"

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/SfuAttachFanout.h"
#include "domain/messaging/SoftMigrateLogic.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <filesystem>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

class FakeTopologyHost final : public CallTopologyHost {
public:
  Roe<std::string> TopologyLocalIdentity() const override {
    if (local_identity.empty()) {
      return Error("no local identity");
    }
    return local_identity;
  }

  Roe<void> TopologyLeaveCall(const std::string& call_id) override {
    leave_calls.push_back(call_id);
    return {};
  }

  Roe<void> TopologyFanOutToJoined(const std::string& call_id, CallControlType type,
                                   const std::string& detail_json, const std::string& /*display*/,
                                   const std::string& /*skip_identity*/) override {
    FanOut f;
    f.call_id = call_id;
    f.type = type;
    f.detail_json = detail_json;
    fanouts.push_back(std::move(f));
    return {};
  }

  Roe<void> TopologySendDirect(const std::string& peer_identity, CallControlType type,
                               const std::string& detail_json,
                               const std::string& /*display*/) override {
    Direct d;
    d.peer_identity = peer_identity;
    d.type = type;
    d.detail_json = detail_json;
    directs.push_back(std::move(d));
    return {};
  }

  void TopologyNotifyRingChanged() override { ++ring_notifies; }
  void TopologySetLastMediaError(std::string message) override { last_media_error = std::move(message); }
  void TopologySetMediaActivity(std::string message) override { media_activity = std::move(message); }
  void TopologyClearMediaActivity() override { media_activity.clear(); }
  void TopologyNoteMediaAttempted(const std::string& call_id) override {
    media_attempted.push_back(call_id);
  }
  void TopologyBindMediaCallId(const std::string& /*call_id*/) override {}
  void TopologyClearMediaPeerIdentity() override {}
  void TopologyReleaseDirectMedia() override { ++direct_media_releases; }
  void TopologyRequestInboxSync() override { ++inbox_sync_requests; }

  struct FanOut {
    std::string call_id;
    CallControlType type = CallControlType::CallEnded;
    std::string detail_json;
  };
  struct Direct {
    std::string peer_identity;
    CallControlType type = CallControlType::CallEnded;
    std::string detail_json;
  };

  std::string local_identity = "account:B";
  std::vector<std::string> leave_calls;
  std::vector<FanOut> fanouts;
  std::vector<Direct> directs;
  std::vector<std::string> media_attempted;
  std::string last_media_error;
  std::string media_activity;
  int ring_notifies = 0;
  int direct_media_releases = 0;
  int inbox_sync_requests = 0;
};

class FakeDialRegistry final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }

  bool IsDialable(const std::string& peer_key) const override {
    return endpoints.find(peer_key) != endpoints.end() || force_dialable.count(peer_key) > 0;
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    const auto it = endpoints.find(peer_key);
    if (it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& /*peer_key*/) override {}
  void AbortInflightDial(const std::string& /*peer_key*/) override {}
  void ClearCallMediaCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> force_dialable;
};

class FakeMediaRelayClient final : public IMediaRelayClient {
public:
  Roe<std::string> LocalPeerIdBase58() const override {
    return local_peer_id;
  }

  bool IsStarted() const override { return started; }

  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                    const MediaRelayQuoteRequest& request,
                                    int /*timeout_ms*/) override {
    ++quote_calls;
    last_quote_hop = hop_peer_key;
    last_quote_call_id = request.call_id;
    if (!quote_ok) {
      return Error(quote_error);
    }
    MediaRelayQuote q;
    q.ok = true;
    q.quote_id = "quote-" + std::to_string(quote_calls);
    q.a_up_bps = 64000;
    q.rate = quote_rate;
    q.pricing_mode = quote_rate > 0 ? "paid" : "volunteer";
    return q;
  }

  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string& /*hop_peer_key*/,
                                              const std::string& /*quote_id*/,
                                              const std::string& /*call_id*/,
                                              const std::string& /*auth_stub*/,
                                              std::function<void(MediaDataFrame)> /*on_frame*/,
                                              int /*timeout_ms*/) override {
    ++attach_calls;
    if (!attach_ok) {
      return Error(attach_error);
    }
    attached_ = true;
    MediaRelayAttachResult r;
    r.ok = true;
    r.session_token = "tok";
    return r;
  }

  void StartClientFrameReader() override { ++reader_starts; }

  void SetClientTransportLostHandler(std::function<void()> handler) override {
    transport_lost_handler = std::move(handler);
  }

  void FireTransportLost() {
    if (transport_lost_handler) {
      transport_lost_handler();
    }
  }

  Roe<MediaRelayAttachResult> AttachAsLocalHop(
      const std::string& /*call_id*/, std::function<void(MediaDataFrame)> /*on_frame*/) override {
    ++local_hop_calls;
    if (!local_hop_ok) {
      return Error(local_hop_error);
    }
    attached_ = true;
    local_hop_attached_ = true;
    MediaRelayAttachResult r;
    r.ok = true;
    r.session_token = "local-tok";
    return r;
  }

  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) override {
    subscribed_streams.push_back(stream_id);
    subscribed_channels.push_back(channel_id);
    return {};
  }
  Roe<void> SendFrame(const MediaDataFrame& /*frame*/) override { return {}; }
  void Detach() override {
    attached_ = false;
    local_hop_attached_ = false;
    ++detach_calls;
  }
  bool IsAttached() const override { return attached_; }
  bool IsLocalHopAttached() const override { return local_hop_attached_; }

  std::string local_peer_id = "12D3KooWLocal";
  bool started = true;
  bool quote_ok = true;
  double quote_rate = 0.0;
  bool attach_ok = true;
  bool local_hop_ok = true;
  std::string quote_error = "media-relay stream open failed: protocol not supported";
  std::string attach_error = "attach failed";
  std::string local_hop_error = "local hop failed";
  int quote_calls = 0;
  int attach_calls = 0;
  int local_hop_calls = 0;
  int detach_calls = 0;
  int reader_starts = 0;
  std::string last_quote_hop;
  std::string last_quote_call_id;
  bool local_hop_attached_ = false;
  std::vector<uint32_t> subscribed_streams;
  std::vector<uint16_t> subscribed_channels;
  std::function<void()> transport_lost_handler;

private:
  bool attached_ = false;
};

class CallTopologyControllerTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_topo_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    sessions_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    media_ = std::make_unique<CallMediaEngine>();
    host_ = std::make_unique<FakeTopologyHost>();
    dial_ = std::make_unique<FakeDialRegistry>();
    relay_ = std::make_unique<FakeMediaRelayClient>();
    topo_ = std::make_unique<CallTopologyController>(*host_, *sessions_, *contacts_, *media_);

    CallTopologyController::MediaRelayDeps deps;
    deps.relay = relay_.get();
    deps.dial = dial_.get();
    deps.bootstrap_peers = {
        "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
    deps.prefer_contacts = false;
    topo_->SetMediaRelayDeps(std::move(deps));
    dial_->force_dialable["12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"] = true;
  }

  void TearDown() override {
    topo_.reset();
    media_.reset();
    contacts_.reset();
    sessions_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  void SeedJoinedCall(const std::string& call_id, const std::vector<std::string>& identities,
                      int64_t base_joined_at) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread-1";
    session.media_mode = CallMediaMode::Voice;
    session.state = CallSessionState::Active;
    session.created_at = base_joined_at;
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    ASSERT_TRUE(sessions_->UpsertSession(session));
    for (size_t i = 0; i < identities.size(); ++i) {
      CallParticipant p;
      p.call_id = call_id;
      p.identity = identities[i];
      p.state = CallParticipantState::Joined;
      p.joined_at = base_joined_at + static_cast<int64_t>(i);
      ASSERT_TRUE(sessions_->UpsertParticipant(p));
    }
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<CallSessionStore> sessions_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<CallMediaEngine> media_;
  std::unique_ptr<FakeTopologyHost> host_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeMediaRelayClient> relay_;
  std::unique_ptr<CallTopologyController> topo_;
};

TEST_F(CallTopologyControllerTest, LocalJoinedWithoutHintDoesNotQuote) {
  const std::string call_id = "call:wait";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:C";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::LocalJoinedWithoutHint);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, MidCallNonInitiatorDoesNotQuote) {
  const std::string call_id = "call:mid-b";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:B";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, MidCallInitiatorPicksAndQuotes) {
  const std::string call_id = "call:mid";
  // A earliest (sticky initiator / payer); picks on roster or accept.
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  // Exercise remote hop path (PreferLocal would short-circuit to AttachAsLocalHop).
  relay_->started = false;

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  EXPECT_GE(relay_->quote_calls, 1);
  EXPECT_EQ(relay_->last_quote_call_id, call_id);
  if (ok) {
    EXPECT_GE(relay_->attach_calls, 1);
    EXPECT_GE(relay_->reader_starts, 1);
    ASSERT_FALSE(host_->fanouts.empty());
    EXPECT_EQ(host_->fanouts.back().type, CallControlType::CallSfuAttach);
    auto decoded = CallControlCodec::DecodeSfuAttach(host_->fanouts.back().detail_json);
    ASSERT_TRUE(decoded);
    EXPECT_TRUE(decoded->quote_id.empty()) << "fan-out must not reuse consumed quote_id";
    EXPECT_FALSE(decoded->hop_peer_id.empty());
  }
}

TEST_F(CallTopologyControllerTest, PickFailsSurfacesStreamOpenError) {
  const std::string call_id = "call:fail";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = false;
  relay_->quote_ok = false;
  relay_->quote_error = "media-relay stream open failed: protocol not supported";

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved);
  ASSERT_FALSE(ok);
  // SoftMigrate returns user-facing copy; technical hop detail stays in logs.
  EXPECT_FALSE(ok.error().message.empty());
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, PaidQuoteBlockedWithoutPaymentRails) {
  const std::string call_id = "call:paid";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = false;
  relay_->quote_rate = 1.25;

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  ASSERT_FALSE(ok);
  EXPECT_GE(relay_->quote_calls, 1);
  EXPECT_EQ(relay_->attach_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
  // SoftMigrateNoHopMessage: all payment_unavailable → dedicated media copy (key or localized).
  EXPECT_TRUE(ok.error().message.find("payment_unavailable_media") != std::string::npos ||
              ok.error().message.find("require payment") != std::string::npos)
      << ok.error().message;
}

TEST_F(CallTopologyControllerTest, PreferLocalOwnerHopOverInCallContact) {
  const std::string call_id = "call:local-hop";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = true;

  // In-call contact B is dialable — PreferInCall would pick B without PreferLocal.
  Contact b;
  b.id = "contact-b";
  b.display_name = "B";
  b.ids = {{ContactIdKind::Account, "account:B", true},
           {ContactIdKind::PeerId, "12D3KooWInCallB", false}};
  b.multiaddrs = {"/ip4/10.0.0.2/tcp/1/p2p/12D3KooWInCallB"};
  PromoteFlatFieldsToNested(b);
  SyncContactMirrors(b);
  ASSERT_TRUE(contacts_->Upsert(b));
  dial_->endpoints["12D3KooWInCallB"] = b.multiaddrs.front();

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_contacts = true;
  deps.prefer_local_as_hop = true;
  deps.local_advertise_multiaddrs = {
      "/ip4/10.0.0.1/tcp/18517/p2p/" + relay_->local_peer_id};
  topo_->SetMediaRelayDeps(std::move(deps));

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(relay_->local_hop_calls, 1);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_EQ(relay_->reader_starts, 0);
  EXPECT_GE(host_->direct_media_releases, 1) << "must drop 1:1 call-media after SoftMigrate";
  ASSERT_FALSE(host_->fanouts.empty());
  auto decoded = CallControlCodec::DecodeSfuAttach(host_->fanouts.back().detail_json);
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->hop_peer_id, relay_->local_peer_id);
  EXPECT_FALSE(decoded->hop_multiaddr.empty()) << "remotes need dialable hop_multiaddr";
  EXPECT_NE(decoded->hop_multiaddr.find(relay_->local_peer_id), std::string::npos);
}

TEST_F(CallTopologyControllerTest, EphemeralStartedDoesNotPreferLocalWithoutFlag) {
  const std::string call_id = "call:ephemeral-no-local";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = true; // mobile ephemeral also IsStarted

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_contacts = false;
  deps.prefer_local_as_hop = false;
  deps.bootstrap_peers = {
      "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  deps.local_advertise_multiaddrs = {"/ip4/10.0.0.9/tcp/1/p2p/" + relay_->local_peer_id};
  topo_->SetMediaRelayDeps(std::move(deps));
  dial_->force_dialable["12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"] = true;

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  EXPECT_EQ(relay_->local_hop_calls, 0);
  if (ok) {
    ASSERT_FALSE(host_->fanouts.empty());
    auto decoded = CallControlCodec::DecodeSfuAttach(host_->fanouts.back().detail_json);
    ASSERT_TRUE(decoded);
    EXPECT_NE(decoded->hop_peer_id, relay_->local_peer_id);
  }
}

TEST_F(CallTopologyControllerTest, AttachSelfHopUsesLocalPublisher) {
  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_local_as_hop = true;
  topo_->SetMediaRelayDeps(std::move(deps));
  relay_->started = true;

  CallSfuAttachDetail attach;
  attach.call_id = "call:self";
  attach.hop_peer_id = relay_->local_peer_id;
  attach.hop_multiaddr = "/ip4/127.0.0.1/tcp/1/p2p/" + relay_->local_peer_id;
  auto ok = topo_->AttachLocalToSfu(attach.call_id, attach);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(relay_->local_hop_calls, 1);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_EQ(relay_->attach_calls, 0);
  EXPECT_TRUE(topo_->IsSfuAttached());
}

TEST_F(CallTopologyControllerTest, IceRecoverNonCoordinatorDoesNotQuote) {
  const std::string call_id = "call:ice";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:B"; // coordinator is account:A

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::IceRecover);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
}

TEST_F(CallTopologyControllerTest, HopHintKeepsPreferLocalAndRefusesGuest) {
  const std::string call_id = "call:keep-local";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = true;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_local_as_hop = true;
  deps.local_advertise_multiaddrs = {"/ip4/10.0.0.1/tcp/18517/p2p/" + relay_->local_peer_id};
  deps.bootstrap_peers = {
      "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  topo_->SetMediaRelayDeps(std::move(deps));
  dial_->force_dialable["12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"] = true;

  ASSERT_TRUE(topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved));
  EXPECT_EQ(relay_->local_hop_calls, 1);
  EXPECT_TRUE(relay_->IsLocalHopAttached());
  const int detaches_after_migrate = relay_->detach_calls;

  CallSfuAttachFailedDetail fail;
  fail.call_id = call_id;
  fail.identity = "account:C";
  fail.failed_hop_peer_id = relay_->local_peer_id;
  fail.error = "quote timed out";
  fail.preferred_hop_peer_ids = {"12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  topo_->OnInboundSfuAttachFailed(fail);
  AppRuntime::RunUITasks();

  EXPECT_EQ(relay_->detach_calls, detaches_after_migrate) << "must not Detach PreferLocal";
  EXPECT_TRUE(relay_->IsLocalHopAttached());
  bool refused = false;
  for (const auto& d : host_->directs) {
    if (d.type == CallControlType::CallHopRefuse && d.peer_identity == "account:C") {
      refused = true;
    }
  }
  EXPECT_TRUE(refused);
}

TEST_F(CallTopologyControllerTest, PreferLocalNodePicksEvenWhenNotInitiator) {
  // Dogfood: sticky initiator wrongly = phone; durable Node must still PreferLocal SoftMigrate.
  const std::string call_id = "call:prefer-local-override";
  // B earliest (wrong initiator); A is PreferLocal Node.
  SeedJoinedCall(call_id, {"account:B", "account:A", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = true;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_contacts = false;
  deps.prefer_local_as_hop = true;
  deps.local_advertise_multiaddrs = {"/ip4/10.0.0.1/tcp/18517/p2p/" + relay_->local_peer_id};
  topo_->SetMediaRelayDeps(std::move(deps));

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::RemoteAcceptObserved);
  ASSERT_TRUE(ok) << ok.error().message;
  EXPECT_EQ(relay_->local_hop_calls, 1);
  ASSERT_FALSE(host_->fanouts.empty());
  EXPECT_EQ(host_->fanouts.back().type, CallControlType::CallSfuAttach);
}

TEST_F(CallTopologyControllerTest, PhoneDefersPickHopWhenDurableMediaRelayListed) {
  const std::string call_id = "call:phone-defer";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A"; // earliest = initiator, but phone
  relay_->started = false;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_contacts = false;
  deps.prefer_local_as_hop = false;
  deps.bootstrap_peers = {
      "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"};
  deps.list_media_relay_peers = []() {
    return std::vector<std::string>{"12D3KooWDurableNode"};
  };
  topo_->SetMediaRelayDeps(std::move(deps));
  dial_->force_dialable["12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR"] = true;

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved);
  ASSERT_TRUE(ok);
  EXPECT_EQ(relay_->quote_calls, 0);
  EXPECT_EQ(relay_->local_hop_calls, 0);
  EXPECT_TRUE(host_->fanouts.empty());
}

TEST_F(CallTopologyControllerTest, SoftMigrateRepickDoesNotDetachPreferLocal) {
  const std::string call_id = "call:repick-keep";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:A";
  relay_->started = true;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_local_as_hop = true;
  deps.local_advertise_multiaddrs = {"/ip4/10.0.0.1/tcp/18517/p2p/" + relay_->local_peer_id};
  topo_->SetMediaRelayDeps(std::move(deps));
  dial_->force_dialable["12D3KooWSeedHop"] = true;

  ASSERT_TRUE(topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::JoinedCountObserved));
  const int detaches = relay_->detach_calls;
  const int local_hops = relay_->local_hop_calls;

  auto ok = topo_->MaybeSoftMigrateToSfu(call_id, SoftMigrateTrigger::IceRecover, "12D3KooWSeedHop");
  EXPECT_FALSE(ok);
  EXPECT_EQ(ok.error().message, "keep_prefer_local");
  EXPECT_EQ(relay_->detach_calls, detaches);
  EXPECT_EQ(relay_->local_hop_calls, local_hops);
  EXPECT_TRUE(relay_->IsLocalHopAttached());
}

TEST_F(CallTopologyControllerTest, InboundSfuAttachDeferredWhileSoftMigrateInFlight) {
  // Moto dogfood: SoftMigrate WaitForAttach in flight + inbound CallSfuAttach must not bump
  // migrate_generation_ (Detach mid-AcceptAndAttach → asio UAF).
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();
  AppRuntime::PauseWorkers();

  const std::string call_id = "call:defer-inbound";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:B"; // non-initiator → WaitForAttach

  ASSERT_TRUE(topo_->OnRemoteAcceptJoined(call_id, 3, "account:C"));
  EXPECT_TRUE(topo_->IsSoftMigrateInFlight());

  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
  attach.hop_multiaddr =
      "/ip4/1.2.3.4/tcp/443/p2p/12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
  dial_->endpoints[attach.hop_peer_id] = attach.hop_multiaddr;

  ASSERT_TRUE(topo_->OnInboundSfuAttach(call_id, attach));
  EXPECT_EQ(relay_->attach_calls, 0) << "must defer while SoftMigrate in flight";
  EXPECT_EQ(relay_->detach_calls, 0);

  AppRuntime::ResumeWorkers();
  bool attached = false;
  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (topo_->IsSfuAttached()) {
      attached = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUITasks();
  EXPECT_TRUE(attached) << "FlushPendingInboundSfuAttach should attach after SoftMigrate";
  EXPECT_GE(relay_->attach_calls, 1);
  EXPECT_EQ(relay_->detach_calls, 0);

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

TEST_F(CallTopologyControllerTest, InboundAnnounceSubscribesWithoutRosterPeer) {
  // Samsung dogfood: SyncSfuSubscriptions peers=1 (roster missing Moto) while Moto TX'd.
  // Peer CallSfuAttach publisher_stream_id must still Subscribe.
  const std::string call_id = "call:announce-sub";
  SeedJoinedCall(call_id, {"account:A", "account:B"}, 1000); // only 2 Joined locally
  host_->local_identity = "account:B";
  relay_->started = true;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_local_as_hop = true;
  deps.local_advertise_multiaddrs = {"/ip4/10.0.0.1/tcp/1/p2p/" + relay_->local_peer_id};
  topo_->SetMediaRelayDeps(std::move(deps));

  CallSfuAttachDetail self;
  self.call_id = call_id;
  self.hop_peer_id = relay_->local_peer_id;
  self.hop_multiaddr = "/ip4/10.0.0.1/tcp/1/p2p/" + relay_->local_peer_id;
  self.publisher_stream_id = PublisherStreamIdForIdentity("account:A");
  ASSERT_TRUE(topo_->AttachLocalToSfu(call_id, self)) << "self hop attach";
  ASSERT_TRUE(topo_->IsSfuAttached());

  const uint32_t moto_stream = PublisherStreamIdForIdentity("account:xaug44GAhFLCTTHR");
  CallSfuAttachDetail announce;
  announce.call_id = call_id;
  announce.hop_peer_id = relay_->local_peer_id;
  announce.hop_multiaddr = self.hop_multiaddr;
  announce.publisher_stream_id = moto_stream;
  ASSERT_TRUE(topo_->OnInboundSfuAttach(call_id, announce));

  bool got_moto = false;
  for (uint32_t s : relay_->subscribed_streams) {
    if (s == moto_stream) {
      got_moto = true;
      break;
    }
  }
  EXPECT_TRUE(got_moto) << "must Subscribe announced publisher even when not in Joined roster";
}

TEST_F(CallTopologyControllerTest, GuestReattachOnTransportLost) {
  // Moto dogfood: mid-call hop CleanupParticipant left TX zombie / RX frozen with no recovery.
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  const std::string call_id = "call:guest-reattach";
  SeedJoinedCall(call_id, {"account:A", "account:B", "account:C"}, 1000);
  host_->local_identity = "account:B";
  relay_->started = true;
  relay_->local_peer_id = "12D3KooWLocalGuest";

  const std::string hop = "12D3KooWCmqCKgBL47m25WzUgiAPayf3GqKiRosmPvAqp2MQUFYR";
  dial_->endpoints[hop] = "/ip4/1.2.3.4/tcp/443/p2p/" + hop;

  CallTopologyController::MediaRelayDeps deps;
  deps.relay = relay_.get();
  deps.dial = dial_.get();
  deps.prefer_local_as_hop = false;
  topo_->SetMediaRelayDeps(std::move(deps));
  ASSERT_TRUE(relay_->transport_lost_handler) << "SetMediaRelayDeps must arm transport-lost handler";

  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = hop;
  attach.hop_multiaddr = dial_->endpoints[hop];
  ASSERT_TRUE(topo_->AttachLocalToSfu(call_id, attach));
  ASSERT_TRUE(topo_->IsSfuAttached());
  const int attaches_after_first = relay_->attach_calls;
  const int quotes_after_first = relay_->quote_calls;
  EXPECT_GE(attaches_after_first, 1);
  EXPECT_GE(relay_->reader_starts, 1);

  relay_->FireTransportLost();
  AppRuntime::RunUITasks();

  bool reattached = false;
  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (relay_->attach_calls > attaches_after_first) {
      reattached = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUITasks();
  EXPECT_TRUE(reattached) << "guest duplex loss must re-AcceptAndAttach";
  EXPECT_GT(relay_->quote_calls, quotes_after_first);
  EXPECT_GT(relay_->reader_starts, 1);
  EXPECT_TRUE(topo_->IsSfuAttached());

  AppRuntime::Shutdown();
  AppRuntime::ShutdownUI();
}

} // namespace
} // namespace pbr
