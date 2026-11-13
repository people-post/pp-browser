#include "lib/calls/CallSessionLogic.h"
#include "lib/calls/CallSummaryLogic.h"
#include "lib/calls/testing/FailingCallSessionStore.h"
#include "lib/calls/testing/InMemoryCallStores.h"
#include "lib/calls/CallLifecycleTypes.h"
#include "feature/calls/tests/calls_loopback_outbox.h"
#include "feature/calls/tests/calls_on_owner.h"
#include "feature/calls/CallDirectPath.h"
#include "lib/calls/CallMediaSeat.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallHopPath.h"
#include "feature/calls/CallHopRelayDeps.h"
#include "lib/calls/CallLifecycleTypes.h"

#include "lib/media/MediaEngine.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallMediaKeyExchange.h"
#include "lib/calls/CallGuests.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/calls/ICallSessionStore.h"
#include "lib/calls/CallTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/ValueJson.h"
#include "domain/call_media/ICallMediaTransport.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/runtime/AppRuntime.h"
#include "common/media/MediaSessionsOwner.h"
#include "common/Utilities.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <atomic>
#include <optional>
#include "feature/calls/tests/call_media_inbound_fake.h"
#include "feature/calls/tests/call_direct_driver_fake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xc0 + i);
  }
  return dek;
}

ByteVector TestMediaKey() {
  ByteVector key(32);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(0x20 + i);
  }
  return key;
}

class FakeDialRegistry final : public mesh::IDialRegistry {
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
  void ClearPeerCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> force_dialable;
};

class FakeCallMediaTransport final : public ICallMediaTransport {
public:
  void Start() override { started = true; }
  void Stop() override { started = false; }
  void SetInboundHandler(CallMediaInboundHandler handler) override { inbound.Set(std::move(handler)); }
  void ClearInboundHandler() override { inbound.Clear(); }
  bool IsActive() const override { return active; }
  CallMediaDirectConnectParams ActiveParams() const override { return active_params; }
  CallMediaSessionPhase Phase() const override {
    return active ? CallMediaSessionPhase::MediaReady : CallMediaSessionPhase::Idle;
  }
  void Detach(const std::string& /*call_id*/) override {
    active = false;
    ++detach_calls;
  }
  void DetachAll() override { Detach({}); }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(CallMediaRoe)> on_done, int /*timeout_ms*/) override {
    ++connect_async_calls;
    last_params = params;
    active = true;
    active_params = params;
    if (callbacks.on_connected) {
      callbacks.on_connected();
    }
    if (on_done) {
      on_done({});
    }
  }
  CallMediaRoe Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                       int timeout_ms) override {
    CallMediaRoe out;
    ConnectAsync(params, std::move(callbacks), [&](CallMediaRoe r) { out = std::move(r); }, timeout_ms);
    return out;
  }
  Roe<void> SendAudio(const std::vector<uint8_t>& /*opus*/, uint32_t /*seq*/, uint8_t /*mark*/) override { return {}; }
  Roe<void> SendMedia(uint8_t /*channel*/, const std::vector<uint8_t>& /*payload*/, uint32_t /*seq*/, uint8_t /*mark*/,
                      uint64_t /*instance*/, uint32_t /*capture_ts*/) override {
    return {};
  }

  bool started = false;
  bool active = false;
  int connect_async_calls = 0;
  int detach_calls = 0;
  CallMediaDirectConnectParams last_params;
  CallMediaDirectConnectParams active_params;
  test::InboundHelloFake inbound;
};

/** Unwrap a sent CallAccept's control-message payload_json ({"control_type":..,"detail":..}) and
 *  decode its detail — mirrors CallSessionManager::SendCallDirectMessage's wire wrapping. */
Roe<CallAcceptDetail> DecodeSentAccept(const std::string& payload_json) {
  auto payload = TryParseObject(payload_json);
  auto detail_json = payload ? payload->getString("detail") : std::nullopt;
  if (!detail_json) {
    return Error("sent payload missing detail");
  }
  return CallControlCodec::DecodeAccept(*detail_json);
}

void DrainUntil(const std::function<bool()>& done, int max_ms = 4000) {
  const int slices = std::max(1, max_ms / 10);
  for (int i = 0; i < slices; ++i) {
    AppRuntime::RunUIAndOwnerTasks();
    if (done()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUIAndOwnerTasks();
}

class CallSessionInboundComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(MediaSessionsManualRuntimeConfig());
    AppRuntime::InitializeUI();

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_csm_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);

    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    sessions_ = std::make_unique<FailingCallSessionStore>();
    keys_ = std::make_unique<InMemoryCallMediaKeyStore>();
    ASSERT_TRUE(keys_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "csm-test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    auto loaded = identity_->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    local_identity_ = loaded->account_id;
    ASSERT_FALSE(local_identity_.empty());

    media_ = std::make_unique<MediaEngine>();
    media_->SetSkipDeviceOpenForTest(true);
    dial_ = std::make_unique<FakeDialRegistry>();
    transport_ = std::make_unique<FakeCallMediaTransport>();

    CallDeliveryPorts delivery;
    delivery.send_control = [this](const std::string& peer, const ThreadMessage& control, bool urgent) -> Roe<void> {
      if (fail_sends_ || peer == unreachable_peer_) {
        return Error("offline (test)");
      }
      const ThreadMessage& msg = control;
      ++sent_control_messages_;
      last_sent_payload_ = msg.payload_json;
      if (auto payload = TryParseObject(msg.payload_json); payload) {
        const auto type = payload->getString("control_type");
        if (type == "call_accept") {
          last_sent_accept_payload_ = msg.payload_json;
        } else if (type == "call_roster") {
          ++sent_rosters_;
        } else if (type == "call_invite") {
          invite_urgent_.push_back(urgent);
          last_sent_invite_payload_ = msg.payload_json;
        } else if (type == "call_ended") {
          ++sent_ended_;
        }
      }
      return {};
    };
    delivery.send_control_to_many = ControlSendsOneByOne(delivery.send_control);
    delivery.send_to_thread = [this](const std::string& thread_id, const std::string& text,
                                     const SendRelayOptions& options) -> Roe<ThreadMessage> {
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.sender_contact_id = kLocalSelfContactId;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      msg.payload_json = options.payload_json.value_or("");
      summaries_.push_back(msg);
      return msg;
    };
    delivery.resolve_peer_thread = [](const std::string& peer) -> Roe<std::string> { return "dm:" + peer; };
    delivery.sync_inbox_from_wake = [this](bool /*force*/) { ++inbox_syncs_; };
    delivery.grant_call_peers = [this](std::vector<std::string> accounts) {
      granted_.insert(granted_.end(), accounts.begin(), accounts.end());
    };
    if (with_group_standing_) {
      // V062: the group's roster as this device holds it (`holds_roster_` false: a guest's device, none held).
      delivery.group_standing = [this](const std::string&,
                                       const std::string& account) -> std::optional<CallGroupStanding> {
        if (!holds_roster_) {
          return std::nullopt;
        }
        auto it = standings_.find(account);
        return it == standings_.end() ? CallGroupStanding{} : it->second;
      };
    }

    reads_ = std::make_unique<CallReads>(*sessions_, *identity_, *store_, delivery.group_members, nullptr,
                                         delivery.group_standing);
    device_ = std::make_unique<CallDevice>(*store_, *store_, *contacts_, *identity_, *sessions_, *reads_, *keys_,
                                           std::move(delivery), *media_);
    csm_ = std::make_unique<CallSessionManager>(*device_);
    device_->SetOutbox(csm_events_.Get()); // the fixture plays the stack
    // The manager owns the 1:1 path, as in the product: attach it on the fake transport.
    CallDirectPathDeps direct;
    direct.transport = transport_.get();
    direct.dial = dial_.get();
    direct.lane = AppRuntime::MakeLaneForTesting(); // stands in for the mesh's io lane
    device_->AttachDirectPath(std::move(direct));

    dial_->force_dialable["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWPeer";
  }

  void TearDown() override {
    if (csm_) {
      // Brief wait so Connect/StartDuplex workers release profile.db before remove_all (Windows).
      OnCallsOwner([&] { return device_->PrepareDirectPathForStop(500); });
    }
    // Always Stop — StartDuplex may arm capture after PrepareForTeardown cleared media_call_id_.
    if (media_) {
      media_->Stop();
    }
    // Drain UI/worker replies while CSM/bridge still alive (avoid UAF on late Accept/Decline).
    (void)AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000));
    // Drain only proves a marker task passed; a worker task posted by the body (offerer Accept
    // roster fan-out → SendCallDirectMessage → the thread store) can still be running on
    // another pool thread. Join the pool before destroying anything it may touch — ASan:
    // heap-use-after-free in the thread store's UpsertThread from FanOutToJoinedAndRinging
    // (Windows CI SEGFAULT in InboundAcceptAsOffererSchedulesDirectMedia).
    AppRuntime::Shutdown();
    csm_.reset();
    device_.reset();
    reads_.reset();
    transport_.reset();
    dial_.reset();
    media_.reset();
    if (keys_) {
      keys_->ClearDek();
    }
    keys_.reset();
    sessions_.reset();
    identity_.reset();
    contacts_.reset();
    store_.reset();
    AppRuntime::ShutdownUI();
    // Never throw from TearDown — Windows "file in use" must not abort the suite.
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  Roe<ThreadMessage> MakeInviteMessage(const std::string& call_id, std::optional<int64_t> expires_at = std::nullopt,
                                       std::optional<bool> video_allowed = std::nullopt) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.video_allowed = video_allowed.value_or(false);
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.listen_multiaddrs = {"/ip4/10.0.0.2/tcp/4001/p2p/12D3KooWPeer"};
    invite.libp2p_peer_id = "12D3KooWPeer";
    if (expires_at) {
      invite.expires_at = *expires_at;
    } else {
      invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    }
    auto detail = CallControlCodec::EncodeInvite(invite);
    if (!detail) {
      return detail.error();
    }
    return CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallInvite, "Incoming call", *detail,
                                                "account:peer");
  }

  /** Answerer Invite → AcceptClicked → Leave → Ended. Returns false on assertion failure path. */
  void RunAnswererInviteAcceptLeave(const std::string& call_id) {
    auto msg = MakeInviteMessage(call_id);
    ASSERT_TRUE(msg) << msg.error().message;
    ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
    ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

    OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
    OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
    DrainUntil([&]() {
      return device_->Live().Phase() == CallPhase::JoinedLocal || device_->Live().Phase() == CallPhase::MediaPending ||
             device_->Live().Phase() == CallPhase::MediaConnecting || device_->Live().Phase() == CallPhase::InCall ||
             media_->IsActive();
    });
    ASSERT_NE(device_->Live().Phase(), CallPhase::Accepting) << device_->LastError();
    ASSERT_NE(device_->Live().Phase(), CallPhase::Ringing) << device_->LastError();
    EXPECT_TRUE(device_->Live().MediaAttempted(call_id));

    OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id); });
    EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
    DrainUntil([&]() {
      auto session = sessions_->LoadSession(call_id);
      return session && session->has_value() && (*session)->state == CallSessionState::Ended;
    });
    auto session = sessions_->LoadSession(call_id);
    ASSERT_TRUE(session && session->has_value());
    EXPECT_EQ((*session)->state, CallSessionState::Ended);
    DrainUntil([&]() {
      auto after = reads_->ActiveLocalCall();
      return after && !after->has_value();
    });
  }

  /** A connected call with the peer seeded in the store and the live calls (`group`: a group's call). */
  void SeedConnectedCall(const std::string& call_id, const bool group,
                         const std::vector<std::string>& peers = {"account:peer"}) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread:1";
    if (group) {
      session.origin_group_id = "group:1";
    }
    session.media_mode = CallMediaMode::Voice;
    session.state = CallSessionState::Active;
    session.created_at = util::NowUnixMs();
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    session.initiator_identity = local_identity_; // placed here (AdmitPlaced below)
    ASSERT_TRUE(sessions_->UpsertSession(session));
    std::vector<std::string> everyone = peers;
    everyone.push_back(local_identity_);
    for (const std::string& who : everyone) {
      CallParticipant row;
      row.call_id = call_id;
      row.identity = who;
      row.state = CallParticipantState::Joined;
      row.joined_at = session.created_at;
      ASSERT_TRUE(sessions_->UpsertParticipant(row));
    }
    ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
    device_->LiveCallsForTest().AdmitPlaced(call_id, peers); // as StartCall does
    OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
    OnCallsOwner([&] { device_->LiveCallsForTest().NoteMediaConnected(call_id); });
    ASSERT_EQ(device_->Live().Phase(), CallPhase::InCall);
  }

  Roe<void> PeerLeaves(const std::string& call_id, const std::string& who) {
    CallLeaveDetail leave;
    leave.call_id = call_id;
    leave.identity = who;
    auto detail = CallControlCodec::EncodeLeave(leave);
    if (!detail) {
      return detail.error();
    }
    auto msg = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallLeave, "Left", *detail, who);
    if (!msg) {
      return msg.error();
    }
    return OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, who); });
  }

  /** The live call's peers (empty: no live call). */
  std::vector<std::string> LivePeers(const std::string& call_id) {
    const LiveCall* call = device_->Live().Find(call_id);
    return call ? call->Peers() : std::vector<std::string>{};
  }

  uint32_t MediaEpoch(const std::string& call_id) {
    auto loaded = sessions_->LoadSession(call_id);
    return loaded && loaded->has_value() ? (*loaded)->media_epoch : 0;
  }

  void Sweep() {
    OnCallsOwner([&] { csm_->SweepExpiredInvites(); });
  }

  CallSessionState SessionState(const std::string& call_id) {
    auto loaded = sessions_->LoadSession(call_id);
    return loaded && loaded->has_value() ? (*loaded)->state : CallSessionState::Ended;
  }

  /** The call the device shows (active, else the ring); empty when idle. */
  std::string ShownCallId() const {
    const LiveCall* shown = device_->Live().Shown();
    return shown ? shown->Id() : std::string{};
  }

  void SeedOffererRingingCall(const std::string& call_id, bool video_allowed = false) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread:out";
    session.media_mode = CallMediaMode::Voice;
    session.video_allowed = video_allowed;
    session.state = CallSessionState::Ringing;
    session.created_at = util::NowUnixMs();
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    ASSERT_TRUE(sessions_->UpsertSession(session));
    CallParticipant self;
    self.call_id = call_id;
    self.identity = local_identity_;
    self.state = CallParticipantState::Joined;
    self.joined_at = session.created_at;
    ASSERT_TRUE(sessions_->UpsertParticipant(self));
    CallParticipant peer;
    peer.call_id = call_id;
    peer.identity = "account:peer";
    peer.state = CallParticipantState::Ringing;
    ASSERT_TRUE(sessions_->UpsertParticipant(peer));
    ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
    device_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"}); // as StartCall does
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  /** The in-memory rows; their reads can be made to fail (DEV/PLAN-call-ui-never-detached.md). */
  std::unique_ptr<FailingCallSessionStore> sessions_;
  std::unique_ptr<InMemoryCallMediaKeyStore> keys_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<MediaEngine> media_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  /** The manager's 1:1 path (owned by csm_). */
  std::unique_ptr<CallReads> reads_;
  std::unique_ptr<CallDevice> device_;
  std::unique_ptr<CallSessionManager> csm_;
  CallsLoopbackOutbox<SessionEvent> csm_events_{[this](SessionEvent& event) {
    if (csm_) {
      device_->Handle(event);
    }
  }};
  std::string local_identity_;
  int sent_control_messages_ = 0;
  /** Every call-control send fails (e.g. the relay is unreachable). */
  bool fail_sends_ = false;
  /** Sends to this peer fail (no route). */
  std::string unreachable_peer_;
  std::atomic<int> inbox_syncs_{0}; // bumped from the deferred-key poll on workers
  std::string last_sent_payload_;
  /** The last CallAccept on the wire (the answerer start may send more after it). */
  std::string last_sent_accept_payload_;
  int sent_rosters_ = 0;
  /** Who the calls asked to get our relay grant (multi-relay MR005), in order. */
  std::vector<std::string> granted_;
  /** CallEnded put on the wire. */
  int sent_ended_ = 0;
  /** `urgent` of every CallInvite put on the wire, in order. */
  std::vector<bool> invite_urgent_;
  std::string last_sent_invite_payload_;
  std::vector<ThreadMessage> summaries_;
  /** V062: bind the group standing port (set before SetUp); the roster it reads; whether one is held here. */
  bool with_group_standing_ = false;
  bool holds_roster_ = true;
  std::map<std::string, CallGroupStanding> standings_;
};

TEST_F(CallSessionInboundComposeTest, InboundInviteCreatesPendingAndRingingSession) {
  const std::string call_id = "call:invite-1";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg) << msg.error().message;

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); })) << "inbound invite";

  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, call_id);
  EXPECT_EQ((*pending)->inviter_identity, "account:peer");
  EXPECT_EQ((*pending)->status, "pending");

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Ringing);

  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Ringing);
}

// PB-054: an invite names its sender as the inviter. One sent by account:mallory naming account:peer
// would ring as peer and make peer the call's initiator: it does not ring at all.
TEST_F(CallSessionInboundComposeTest, AnInviteNamingSomeoneElseAsTheCallerDoesNotRing) {
  const std::string call_id = "call:spoofed-inviter";
  auto msg = MakeInviteMessage(call_id); // inviter_identity "account:peer"
  ASSERT_TRUE(msg) << msg.error().message;

  (void)OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:mallory"); });

  auto pending = reads_->TopPendingInvite();
  EXPECT_FALSE(pending && pending->has_value()) << "no ring for a forged caller";
  auto session = sessions_->LoadSession(call_id);
  EXPECT_FALSE(session && session->has_value());
}

TEST_F(CallSessionInboundComposeTest, StaleInviteDroppedByRelayAge) {
  const std::string call_id = "call:stale";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);

  const int64_t created = 1'000;
  const int64_t relay_now = created + kDefaultCallInviteTtlMs + kCallInviteRelayAgeSlackMs + 1;
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer", created, relay_now); }));

  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

// B24: an invite read after the call ended here (its leave came first, or a replay) rings nothing.
TEST_F(CallSessionInboundComposeTest, AReplayedInviteForACallAlreadyEndedRingsNothing) {
  const std::string call_id = "call:over";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->DeclineInvite(call_id); }));

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); })); // the replay
  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, DeclineClearsPendingInvite) {
  const std::string call_id = "call:decline";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->DeclineInvite(call_id); })) << "decline";
  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
  EXPECT_GE(sent_control_messages_, 1);
}

// V037 intents: a second Accept click is not a second accept (the accept no longer waits for the
// circuit park, so it has already gone through by the second click).
TEST_F(CallSessionInboundComposeTest, AcceptClickedDedupesWhileInFlight) {
  const std::string call_id = "call:dedupe";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  int parks = 0;
  device_->MeshGlue().park_circuit = ([&](int /*timeout_ms*/, std::function<void(bool)> /*done*/) { ++parks; });

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, {}); }); // the ring's call
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  AppRuntime::RunAllOwnerTasks();
  EXPECT_EQ(parks, 1) << "the second click started no second accept";
  EXPECT_TRUE(device_->AcceptingCallId().empty());
  EXPECT_NE(device_->Live().Phase(), CallPhase::Ringing);
  EXPECT_TRUE(device_->LastError().empty()) << device_->LastError();
}

// PR #239 review: two inbound bundles can each raise PeerReconnected before the first queued resume
// runs. Only a failed call resumes; a resume that fails after the call went live again must not
// knock it back to ConnectFailed.
TEST_F(CallSessionInboundComposeTest, ADuplicateResumeThatFailsLeavesTheResumedCallInCall) {
  const std::string call_id = "call:resume-dup";
  SeedOffererRingingCall(call_id);
  LiveCalls& live = device_->LiveCallsForTest();
  live.MarkJoined(call_id);
  live.NoteMediaConnected(call_id);
  live.NoteMediaFailed(call_id);
  ASSERT_EQ(device_->Live().Phase(), CallPhase::ConnectFailed);

  // What the call's 1:1 path reports when the peer's connection for the failed call reaches it.
  const auto peer_reconnected = [&] {
    CallMedia* media = device_->CallMediaOf().Media(call_id);
    ASSERT_TRUE(media && media->Direct());
    SessionEvent report{
        session_event::FromDirectPath{call_id, media->DirectSerial(), direct_report::PeerReconnected{call_id}}};
    device_->Handle(report);
  };
  OnCallsOwner(peer_reconnected);
  OnCallsOwner(peer_reconnected);   // ignored: no longer failed
  live.NoteMediaConnected(call_id); // the first resume committed the peer's stream
  AppRuntime::RunAllOwnerTasks();   // the queued resume finds nothing to resume and fails
  EXPECT_EQ(device_->Live().Phase(), CallPhase::InCall);
}

// Accept asks for the circuit park but does not wait for it: a seed silently gone held every
// answer for the whole park (≤ 12 s). The accept commits at once; the park lands when it lands.
TEST_F(CallSessionInboundComposeTest, AcceptParksWithoutWaitingForThePark) {
  const std::string call_id = "call:park";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  std::function<void(bool)> park_done;
  device_->MeshGlue().park_circuit =
      ([&](int /*timeout_ms*/, std::function<void(bool)> done) { park_done = std::move(done); });
  const Roe<void> accepted = OnCallsOwner([&] { return csm_->AcceptInvite(call_id); });
  AppRuntime::RunAllOwnerTasks();
  ASSERT_TRUE(park_done) << "accept asked for the circuit park";
  EXPECT_TRUE(accepted) << accepted.error().message; // the accept does not wait for the park
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
  EXPECT_GE(sent_control_messages_, 1) << "CallAccept sent before the park answered";

  OnCallsOwner([&] { park_done(false); }); // a late (failed) park changes nothing
  AppRuntime::RunAllOwnerTasks();
  self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
}

TEST_F(CallSessionInboundComposeTest, InviteAcceptLeaveProductCompose) {
  // B-CALL-DIRECT product glue: Invite → AcceptClicked → media → Leave → Idle.
  const std::string call_id = "call:compose";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Ringing);

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  // The accept runs on the calls owner at once (no park here): the ring is gone, the call connecting.
  EXPECT_NE(device_->Live().Phase(), CallPhase::Ringing);
  EXPECT_NE(device_->Live().Phase(), CallPhase::Idle);

  DrainUntil([&]() {
    return device_->Live().Phase() == CallPhase::JoinedLocal || device_->Live().Phase() == CallPhase::MediaPending ||
           device_->Live().Phase() == CallPhase::MediaConnecting || device_->Live().Phase() == CallPhase::InCall ||
           media_->IsActive();
  });

  EXPECT_NE(device_->Live().Phase(), CallPhase::Accepting) << device_->LastError();
  EXPECT_NE(device_->Live().Phase(), CallPhase::Ringing) << device_->LastError();
  EXPECT_TRUE(device_->Live().MediaAttempted(call_id));

  auto active = reads_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, call_id);

  // Duplicate invite while Joined must not demote to Ringing.
  auto again = MakeInviteMessage(call_id);
  ASSERT_TRUE(again);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*again, "account:peer"); }));
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Ended);

  DrainUntil([&]() {
    auto after = reads_->ActiveLocalCall();
    return after && !after->has_value();
  });
  auto after = reads_->ActiveLocalCall();
  ASSERT_TRUE(after);
  EXPECT_FALSE(after->has_value());
}

TEST_F(CallSessionInboundComposeTest, InviteAcceptLeaveKCycleTeardown) {
  // B-TEARDOWN: Leave→Idle then a second Invite→Accept→Leave must succeed (no orphan bind).
  RunAnswererInviteAcceptLeave("call:cycle-1");
  ASSERT_EQ(device_->Live().Phase(), CallPhase::Idle);
  EXPECT_FALSE(device_->Seat().IsBound("call:cycle-1"));

  RunAnswererInviteAcceptLeave("call:cycle-2");
  ASSERT_EQ(device_->Live().Phase(), CallPhase::Idle);
  auto after = reads_->ActiveLocalCall();
  ASSERT_TRUE(after);
  EXPECT_FALSE(after->has_value());
}

TEST_F(CallSessionInboundComposeTest, WireSkewStaleInviteDropped) {
  const std::string call_id = "call:wire-stale";
  const int64_t expires = util::NowUnixMs() - kCallInviteWireSkewSlackMs - 1;
  auto msg = MakeInviteMessage(call_id, expires);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, InboundCallEndedEndsActiveSession) {
  const std::string call_id = "call:ended";
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = "thread:1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(sessions_->UpsertSession(session));
  CallParticipant local;
  local.call_id = call_id;
  local.identity = local_identity_;
  local.state = CallParticipantState::Joined;
  local.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(local));
  CallParticipant peer; // the peer who ends it is in the call
  peer.call_id = call_id;
  peer.identity = "account:peer";
  peer.state = CallParticipantState::Joined;
  peer.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(peer));

  device_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"}); // as StartCall does
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteMediaConnected(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::InCall);

  CallEndedDetail ended;
  ended.call_id = call_id;
  ended.duration_ms = 1500;
  auto detail = CallControlCodec::EncodeEnded(ended);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallEnded, "Call ended", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle)
      << "CallEnded → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(ShownCallId().empty());
}

TEST_F(CallSessionInboundComposeTest, InboundAcceptAsOffererSchedulesDirectMedia) {
  // B-CALL-DIRECT offerer glue only: inbound CallAccept → schedule_start(..., offerer=true)
  // + session/peer Joined. Do not drive Bridge BeginSession / StartDuplex / Connect grace —
  // those belong to Bridge/engine tests (and TearDown must not inherit an armed Connect).
  const std::string call_id = "call:offerer-accept";
  SeedOffererRingingCall(call_id);

  std::string scheduled_call;
  std::string scheduled_peer;
  bool scheduled_offerer = false;
  int schedule_calls = 0;
  // The call's coordinator starts the 1:1 path through the direct driver: spy on that.
  struct SpyDriver final : test::NullDirectDriver {
    std::function<void(const std::string&, const std::string&, bool)> on_start;
    void ScheduleDirectStart(const std::string& cid, const std::string& peer, bool offerer) override {
      on_start(cid, peer, offerer);
    }
  } spy;
  spy.on_start = [&](const std::string& cid, const std::string& peer, bool offerer) {
    ++schedule_calls;
    scheduled_call = cid;
    scheduled_peer = peer;
    scheduled_offerer = offerer;
  };
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest(test::ForwardTo(spy)); });

  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  device_->LiveCallsForTest().SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, "test");

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  accept.listen_multiaddrs = {"/ip4/10.0.0.2/tcp/4001/p2p/12D3KooWPeer"};
  accept.libp2p_peer_id = "12D3KooWPeer";
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  EXPECT_EQ(schedule_calls, 1);
  EXPECT_EQ(scheduled_call, call_id);
  EXPECT_EQ(scheduled_peer, "account:peer");
  EXPECT_TRUE(scheduled_offerer);
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest({}); }); // the paths forwarding to the spy go

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Active);
  auto peer = sessions_->FindParticipant(call_id, "account:peer");
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ((*peer)->state, CallParticipantState::Joined);
}

// H013: the callee's CallAccept says where it is parked, so the caller's circuit tries those first.
TEST_F(CallSessionInboundComposeTest, AcceptAdvertisesTheRelaysWeAreParkedOn) {
  const std::string call_id = "call:advertise-parked";
  OnCallsOwner([&] {
    device_->MeshGlue().parked_relays = ([]() { return std::vector<std::string>{"QmRelayA", "QmRelayB"}; });
    return Roe<void>{};
  });
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  const Roe<void> accepted = OnCallsOwner([&] { return csm_->AcceptInvite(call_id); });
  ASSERT_TRUE(accepted) << accepted.error().message;

  auto sent = DecodeSentAccept(last_sent_accept_payload_);
  ASSERT_TRUE(sent);
  EXPECT_EQ(sent->circuit_parked, (std::vector<std::string>{"QmRelayA", "QmRelayB"}));
}

// H013: the caller keeps what the answerer advertised for the call's reach.
TEST_F(CallSessionInboundComposeTest, InboundAcceptReportsTheAnswerersParkedRelays) {
  const std::string call_id = "call:peer-parked";
  SeedOffererRingingCall(call_id);
  test::NullDirectDriver spy;
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest(test::ForwardTo(spy)); });
  std::string noted_call;
  std::vector<std::string> noted_relays;
  OnCallsOwner([&] {
    device_->MeshGlue().on_peer_parked_relays = ([&](const std::string& cid, const std::vector<std::string>& relays) {
      noted_call = cid;
      noted_relays = relays;
    });
    device_->LiveCallsForTest().NoteOutboundStarted(call_id);
  });

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  accept.circuit_parked = {"QmRelayC", "QmRelayD"};
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest({}); }); // the paths forwarding to the spy go
  EXPECT_EQ(noted_call, call_id);
  EXPECT_EQ(noted_relays, (std::vector<std::string>{"QmRelayC", "QmRelayD"}));
}

// H013: a give-up counts only from the call's own peer.
TEST_F(CallSessionInboundComposeTest, AConnectGiveUpFromSomeoneElseIsIgnored) {
  const std::string call_id = "call:give-up-other";
  auto invite = MakeInviteMessage(call_id);
  ASSERT_TRUE(invite);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*invite, "account:peer"); }));
  CallConnectGiveUpDetail give_up;
  give_up.call_id = call_id;
  give_up.identity = "account:mallory";
  auto detail = CallControlCodec::EncodeConnectGiveUp(give_up);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:x", CallControlType::CallConnectGiveUp, "", *detail,
                                                  "account:mallory");
  ASSERT_TRUE(msg);
  EXPECT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:mallory"); }));
  EXPECT_FALSE(device_->IsDirectConnectFailed());
}

// video-voice-choice: callee answers an incoming video invite as voice-only. AcceptInvite must
// narrow its own session and send Accept.video_allowed=false.
TEST_F(CallSessionInboundComposeTest, VoiceOnlyAnswerNarrowsCalleeSessionAndSendsFalse) {
  const std::string call_id = "call:voice-only-narrow";
  auto msg = MakeInviteMessage(call_id, std::nullopt, /*video_allowed=*/true);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  OnCallsOwner([&] { return csm_->SetPendingAcceptVoiceOnly(true); });
  const Roe<void> accepted = OnCallsOwner([&] { return csm_->AcceptInvite(call_id); });
  EXPECT_TRUE(accepted) << accepted.error().message;

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_FALSE((*session)->video_allowed);

  auto sent = DecodeSentAccept(last_sent_accept_payload_);
  ASSERT_TRUE(sent);
  ASSERT_TRUE(sent->video_allowed.has_value());
  EXPECT_FALSE(*sent->video_allowed);
}

// A voice-only accept on a call that was already voice-only writes nothing extra on the wire.
TEST_F(CallSessionInboundComposeTest, VoiceOnlyAnswerOnVoiceCallWritesNothing) {
  const std::string call_id = "call:voice-only-noop";
  auto msg = MakeInviteMessage(call_id); // voice invite (video_allowed=false)
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  OnCallsOwner([&] { return csm_->SetPendingAcceptVoiceOnly(true); });
  const Roe<void> accepted = OnCallsOwner([&] { return csm_->AcceptInvite(call_id); });
  EXPECT_TRUE(accepted) << accepted.error().message;

  auto sent = DecodeSentAccept(last_sent_accept_payload_);
  ASSERT_TRUE(sent);
  EXPECT_FALSE(sent->video_allowed.has_value());

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_FALSE((*session)->video_allowed);
}

// Our accept's roster fan-out is a follow-up the owner runs after the accept reported.
TEST_F(CallSessionInboundComposeTest, AcceptFansOutTheRoster) {
  const std::string call_id = "call:roster-after-accept";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->AcceptInvite(call_id); }));
  DrainUntil([&] { return sent_rosters_ > 0; }, 500);
  EXPECT_GT(sent_rosters_, 0);
}

// Multi-relay MR005: answering a call grants the caller (and anyone joined in it), so its signaling to us is a
// contact's on our relay, not a stranger's held to 3 an hour (the lab's stranger calls, grown calls).
TEST_F(CallSessionInboundComposeTest, AnsweringACallGrantsTheCaller) {
  const std::string call_id = "call:grant-on-answer";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  EXPECT_TRUE(granted_.empty()) << "ringing grants nobody: the call is not answered yet";
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->AcceptInvite(call_id); }));
  EXPECT_NE(std::find(granted_.begin(), granted_.end(), "account:peer"), granted_.end());
  EXPECT_EQ(std::find(granted_.begin(), granted_.end(), local_identity_), granted_.end()) << "never ourselves";
}

// MR005: someone joining a call we are in is granted — its own accept, or its own roster saying it joined; a
// roster's word about third parties grants nobody.
TEST_F(CallSessionInboundComposeTest, SomeoneJoiningACallWeAreInIsGrantedByItsOwnWordOnly) {
  const std::string call_id = "call:grant-on-join";
  SeedOffererRingingCall(call_id);
  test::NullDirectDriver null_driver; // the 1:1 start goes nowhere: only the grants are looked at
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest(test::ForwardTo(null_driver)); });
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  accept.libp2p_peer_id = "12D3KooWPeer";
  auto accept_detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(accept_detail);
  auto accepted = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted",
                                                       *accept_detail, "account:peer");
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*accepted, "account:peer"); }));
  EXPECT_EQ(granted_, std::vector<std::string>{"account:peer"}) << "the callee answered our call";

  // Someone in the call invited Carol (their roster made her a ringing participant here); she joined, and her
  // roster, sent by her, names her and us joined, and Dave.
  CallParticipant carol;
  carol.call_id = call_id;
  carol.identity = "account:carol";
  carol.state = CallParticipantState::Ringing;
  ASSERT_TRUE(sessions_->UpsertParticipant(carol));
  CallRosterDetail roster;
  roster.call_id = call_id;
  for (const std::string& who :
       {local_identity_, std::string("account:peer"), std::string("account:carol"), std::string("account:dave")}) {
    CallRosterEntry entry;
    entry.identity = who;
    entry.state = CallParticipantState::Joined;
    roster.participants.push_back(entry);
  }
  auto detail = CallControlCodec::EncodeRoster(roster);
  ASSERT_TRUE(detail);
  auto from_carol = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallRoster, "Call roster",
                                                         *detail, "account:carol");
  ASSERT_TRUE(from_carol);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*from_carol, "account:carol"); }));
  EXPECT_EQ(granted_, (std::vector<std::string>{"account:peer", "account:carol"}))
      << "Carol, by her own roster; not Dave, whom only her roster names";
  OnCallsOwner([&] { device_->SetDirectPathFactoryForTest({}); });
}

// PR #216 follow-up: DropFollowUps (hub teardown, before the store and delivery go) drops a
// roster fan-out the accept already reported but the owner has not run yet.
TEST_F(CallSessionInboundComposeTest, DropFollowUpsDropsAQueuedRosterFanOut) {
  const std::string call_id = "call:roster-dropped";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  bool accepted = false;
  OnCallsOwner([&] {
    accepted = static_cast<bool>(csm_->AcceptInvite(call_id));
    device_->DropFollowUps(); // the roster follow-up is queued, not yet run
  });
  DrainUntil([] { return false; }, 200);
  ASSERT_TRUE(accepted);
  EXPECT_EQ(sent_rosters_, 0) << "a roster queued before DropFollowUps still ran";
}

// A normal (video) accept on a video invite leaves the field unset and keeps video_allowed=true.
TEST_F(CallSessionInboundComposeTest, VideoAnswerLeavesFieldUnset) {
  const std::string call_id = "call:video-answer-normal";
  auto msg = MakeInviteMessage(call_id, std::nullopt, /*video_allowed=*/true);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  const Roe<void> accepted = OnCallsOwner([&] { return csm_->AcceptInvite(call_id); });
  EXPECT_TRUE(accepted) << accepted.error().message;

  auto sent = DecodeSentAccept(last_sent_accept_payload_);
  ASSERT_TRUE(sent);
  EXPECT_FALSE(sent->video_allowed.has_value());

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_TRUE((*session)->video_allowed);
}

// Caller side: a 1:1 outgoing video call whose remote answers voice-only narrows the caller's
// session too (VideoAllowedForCall flips false).
TEST_F(CallSessionInboundComposeTest, CallerNarrowsOnVoiceOnlyAccept) {
  const std::string call_id = "call:caller-narrow";
  SeedOffererRingingCall(call_id, /*video_allowed=*/true);

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  accept.listen_multiaddrs = {"/ip4/10.0.0.2/tcp/4001/p2p/12D3KooWPeer"};
  accept.libp2p_peer_id = "12D3KooWPeer";
  accept.video_allowed = false;
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto video_allowed = reads_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(video_allowed && video_allowed->has_value());
  EXPECT_FALSE(**video_allowed);
}

// A roster from the peer is built from ITS view of the call and can arrive late over the relay
// (device test 2026-09-28: the callee's accept-time roster, listing both cameras off, reached the
// caller 2 s after the caller's camera auto-enabled). Our own entry in it is stale by definition —
// only we know our camera/mic — so it must never overwrite our own participant row.
TEST_F(CallSessionInboundComposeTest, PeerRosterNeverOverwritesOwnMediaState) {
  const std::string call_id = "call:roster-self";
  SeedOffererRingingCall(call_id, /*video_allowed=*/true);
  auto self_row = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self_row && self_row->has_value());
  CallParticipant self = **self_row;
  self.media.video_enabled = true;
  self.media.audio_muted = false;
  ASSERT_TRUE(sessions_->UpsertParticipant(self));

  CallRosterDetail roster;
  roster.call_id = call_id;
  CallRosterEntry stale_self;
  stale_self.identity = local_identity_;
  stale_self.state = CallParticipantState::Joined;
  stale_self.video_enabled = false;
  stale_self.audio_muted = true;
  CallRosterEntry peer;
  peer.identity = "account:peer";
  peer.state = CallParticipantState::Joined;
  peer.video_enabled = true;
  roster.participants = {stale_self, peer};
  auto detail = CallControlCodec::EncodeRoster(roster);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallRoster, "Call roster", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto after = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(after && after->has_value());
  EXPECT_TRUE((*after)->media.video_enabled) << "own camera state came from a stale peer roster";
  EXPECT_FALSE((*after)->media.audio_muted) << "own mute state came from a stale peer roster";
  auto peer_row = sessions_->FindParticipant(call_id, "account:peer");
  ASSERT_TRUE(peer_row && peer_row->has_value());
  EXPECT_TRUE((*peer_row)->media.video_enabled) << "the peer's own entry still applies";
}

// A replayed Accept without the field (relay retransmit) must never re-widen an already-narrowed
// call; a fresh call whose Accept never carries the field stays at the caller's original choice.
TEST_F(CallSessionInboundComposeTest, CallerIgnoresMissingFieldAndNeverRewidens) {
  const std::string call_id = "call:caller-replay";
  SeedOffererRingingCall(call_id, /*video_allowed=*/true);

  CallAcceptDetail narrow_accept;
  narrow_accept.call_id = call_id;
  narrow_accept.identity = "account:peer";
  narrow_accept.video_allowed = false;
  auto narrow_detail = CallControlCodec::EncodeAccept(narrow_accept);
  ASSERT_TRUE(narrow_detail);
  auto narrow_msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted",
                                                         *narrow_detail, "account:peer");
  ASSERT_TRUE(narrow_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*narrow_msg, "account:peer"); }));
  auto after_narrow = reads_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(after_narrow && after_narrow->has_value());
  ASSERT_FALSE(**after_narrow);

  CallAcceptDetail replay_accept; // relay replay of the same Accept, missing the field this time
  replay_accept.call_id = call_id;
  replay_accept.identity = "account:peer";
  auto replay_detail = CallControlCodec::EncodeAccept(replay_accept);
  ASSERT_TRUE(replay_detail);
  auto replay_msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted",
                                                         *replay_detail, "account:peer");
  ASSERT_TRUE(replay_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*replay_msg, "account:peer"); }));
  auto after_replay = reads_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(after_replay && after_replay->has_value());
  EXPECT_FALSE(**after_replay) << "a replayed Accept without video_allowed must not re-widen the call";

  const std::string call_id2 = "call:caller-fresh";
  SeedOffererRingingCall(call_id2, /*video_allowed=*/true);
  CallAcceptDetail plain_accept;
  plain_accept.call_id = call_id2;
  plain_accept.identity = "account:peer";
  auto plain_detail = CallControlCodec::EncodeAccept(plain_accept);
  ASSERT_TRUE(plain_detail);
  auto plain_msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted",
                                                        *plain_detail, "account:peer");
  ASSERT_TRUE(plain_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*plain_msg, "account:peer"); }));
  auto fresh_video_allowed = reads_->VideoAllowedForCall(call_id2);
  ASSERT_TRUE(fresh_video_allowed && fresh_video_allowed->has_value());
  EXPECT_TRUE(**fresh_video_allowed);
}

// A group call (≥2 invitees): a voice-only Accept from one invitee must only affect that
// participant, never the call-wide video_allowed the caller set.
TEST_F(CallSessionInboundComposeTest, GroupVoiceAnswerDoesNotNarrowCaller) {
  const std::string call_id = "call:group-voice-answer";
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = "thread:out";
  session.origin_group_id = "group:out"; // started from a group thread
  session.media_mode = CallMediaMode::Voice;
  session.video_allowed = true;
  session.state = CallSessionState::Ringing;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(sessions_->UpsertSession(session));
  CallParticipant self;
  self.call_id = call_id;
  self.identity = local_identity_;
  self.state = CallParticipantState::Joined;
  self.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(self));
  CallParticipant peer1;
  peer1.call_id = call_id;
  peer1.identity = "account:peer1";
  peer1.state = CallParticipantState::Ringing;
  ASSERT_TRUE(sessions_->UpsertParticipant(peer1));
  CallParticipant peer2;
  peer2.call_id = call_id;
  peer2.identity = "account:peer2";
  peer2.state = CallParticipantState::Ringing;
  ASSERT_TRUE(sessions_->UpsertParticipant(peer2));
  device_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer1", "account:peer2"}); // as StartCall does
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer1";
  accept.video_allowed = false;
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer1");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer1"); }));

  auto video_allowed = reads_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(video_allowed && video_allowed->has_value());
  EXPECT_TRUE(**video_allowed) << "voice-only Accept from one of >=2 invitees must not narrow the whole call";
}

// PR #230 review: narrowing keys on the call's origin, not the live row count. A group-thread call
// with a single invitee (2 rows) stays video-allowed; a direct call still narrows after the caller
// added a guest (3 rows) before the voice-only accept arrived — both sides then agree.
TEST_F(CallSessionInboundComposeTest, VoiceAnswerNarrowingFollowsTheCallOrigin) {
  const std::string group_call = "call:group-one-invitee";
  SeedOffererRingingCall(group_call, /*video_allowed=*/true);
  auto group_session = sessions_->LoadSession(group_call);
  ASSERT_TRUE(group_session && group_session->has_value());
  (*group_session)->origin_group_id = "group:out";
  ASSERT_TRUE(sessions_->UpsertSession(**group_session));

  const std::string direct_call = "call:direct-with-guest";
  SeedOffererRingingCall(direct_call, /*video_allowed=*/true);
  CallParticipant guest;
  guest.call_id = direct_call;
  guest.identity = "account:guest";
  guest.state = CallParticipantState::Ringing;
  ASSERT_TRUE(sessions_->UpsertParticipant(guest));

  for (const std::string& call_id : {group_call, direct_call}) {
    CallAcceptDetail accept;
    accept.call_id = call_id;
    accept.identity = "account:peer";
    accept.video_allowed = false;
    auto detail = CallControlCodec::EncodeAccept(accept);
    ASSERT_TRUE(detail);
    auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                    "account:peer");
    ASSERT_TRUE(msg);
    ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  }
  auto group_allowed = reads_->VideoAllowedForCall(group_call);
  ASSERT_TRUE(group_allowed && group_allowed->has_value());
  EXPECT_TRUE(**group_allowed) << "a group-thread call is never narrowed call-wide";
  auto direct_allowed = reads_->VideoAllowedForCall(direct_call);
  ASSERT_TRUE(direct_allowed && direct_allowed->has_value());
  EXPECT_FALSE(**direct_allowed) << "a direct call narrows even with a guest already invited";
}

TEST_F(CallSessionInboundComposeTest, InboundMediaKeyIsTakenAndKicksAnswerer) {
  const std::string call_id = "call:media-key";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  DrainUntil([&]() {
    return device_->Live().Phase() == CallPhase::JoinedLocal || device_->Live().Phase() == CallPhase::MediaPending ||
           device_->Live().Phase() == CallPhase::MediaConnecting || media_->IsActive();
  });
  // Accept without pre-seeded key → MediaPending / KeyWait.
  EXPECT_TRUE(device_->Live().MediaAttempted(call_id));
  if (media_->IsActive()) {
    // Rare: key race; still exercise the inbound key below.
  } else {
    EXPECT_TRUE(device_->Live().Phase() == CallPhase::MediaPending ||
                device_->Live().Phase() == CallPhase::JoinedLocal ||
                device_->Live().Phase() == CallPhase::MediaConnecting);
  }

  const ByteVector media_key = TestMediaKey();
  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = 1;
  key_detail.media_key_id = "mk:1";
  key_detail.media_key_b64 = Base64Encode(media_key);
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  ASSERT_TRUE(key_json);
  auto key_msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallMediaKey, "Media key",
                                                      *key_json, "account:peer");
  ASSERT_TRUE(key_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*key_msg, "account:peer"); }));

  DrainUntil([&]() { return media_->IsActive(); });
  EXPECT_TRUE(media_->IsActive());
  auto stored = keys_->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(stored && stored->has_value());
  EXPECT_EQ(**stored, media_key);

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id); });
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, InboundPeerLeaveEndsActiveOneToOne) {
  const std::string call_id = "call:peer-leave";
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = "thread:1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  session.media_key_id = "mk:1";
  ASSERT_TRUE(sessions_->UpsertSession(session));

  CallParticipant local;
  local.call_id = call_id;
  local.identity = local_identity_;
  local.state = CallParticipantState::Joined;
  local.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(local));
  CallParticipant peer;
  peer.call_id = call_id;
  peer.identity = "account:peer";
  peer.state = CallParticipantState::Joined;
  peer.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(peer));

  device_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"}); // as StartCall does
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteMediaConnected(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::InCall);
  ASSERT_EQ(ShownCallId(), call_id);

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = "account:peer";
  auto detail = CallControlCodec::EncodeLeave(leave);
  ASSERT_TRUE(detail);
  auto msg =
      CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallLeave, "Left", *detail, "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle)
      << "peer Leave → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(ShownCallId().empty());
  EXPECT_FALSE((device_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, AcceptSecondInviteEndsPriorActiveCall) {
  // B-CONFLICT product glue: Accept B while Joined on A ends A via LeaveCallIfActiveExcept.
  const std::string call_a = "call:conflict-a";
  const std::string call_b = "call:conflict-b";

  auto msg_a = MakeInviteMessage(call_a);
  ASSERT_TRUE(msg_a);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg_a, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_a, 1, TestMediaKey()));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_a); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_a); });
  DrainUntil([&]() {
    auto active = reads_->ActiveLocalCall();
    // Wait for AcceptInvite UI completion — reduces SQLITE_BUSY vs concurrent invite B upsert.
    return active && active->has_value() && (*active)->call_id == call_a &&
           device_->Live().Phase() != CallPhase::Accepting;
  });
  ASSERT_TRUE(reads_->ActiveLocalCall()->has_value());

  auto msg_b = MakeInviteMessage(call_b);
  ASSERT_TRUE(msg_b);
  {
    auto applied_b = OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg_b, "account:peer"); });
    ASSERT_TRUE(applied_b) << (applied_b ? "" : applied_b.error().message);
  }
  ASSERT_TRUE(keys_->PutEpochKey(call_b, 1, TestMediaKey()));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_b); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_b); });
  DrainUntil([&]() {
    auto active = reads_->ActiveLocalCall();
    return active && active->has_value() && (*active)->call_id == call_b;
  });

  auto a = sessions_->LoadSession(call_a);
  ASSERT_TRUE(a && a->has_value());
  EXPECT_EQ((*a)->state, CallSessionState::Ended);
  auto b = sessions_->LoadSession(call_b);
  ASSERT_TRUE(b && b->has_value());
  EXPECT_NE((*b)->state, CallSessionState::Ended);
  auto active = reads_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, call_b);
  // Ending A must not RemoteEnded-clobber Accepting/Joined B.
  EXPECT_NE(device_->Live().Phase(), CallPhase::Idle);
  EXPECT_EQ(ShownCallId(), call_b);

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_b); });
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_b);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, AbandonOrphanedCallsAfterRestartClearsJoinedAndPending) {
  const std::string call_id = "call:orphan";
  CallSession session;
  session.call_id = call_id;
  session.origin_thread_id = "thread:1";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Active;
  session.created_at = util::NowUnixMs();
  session.media_epoch = 1;
  ASSERT_TRUE(sessions_->UpsertSession(session));
  CallParticipant local;
  local.call_id = call_id;
  local.identity = local_identity_;
  local.state = CallParticipantState::Joined;
  local.joined_at = session.created_at;
  ASSERT_TRUE(sessions_->UpsertParticipant(local));

  PendingCallInvite pending;
  pending.call_id = "call:pending-orphan";
  pending.inviter_identity = "account:peer";
  pending.invitee_identity = local_identity_;
  pending.media_mode = CallMediaMode::Voice;
  pending.origin_thread_id = "thread:2";
  pending.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
  pending.created_at = util::NowUnixMs();
  pending.status = "pending";
  ASSERT_TRUE(sessions_->UpsertPendingInvite(pending));

  OnCallsOwner([&] { return csm_->AbandonOrphanedCallsAfterRestart(); });

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  auto top = reads_->TopPendingInvite();
  ASSERT_TRUE(top);
  EXPECT_FALSE(top->has_value());
}

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesMarksMissed) {
  PendingCallInvite pending;
  pending.call_id = "call:expired-sweep";
  pending.inviter_identity = "account:peer";
  pending.invitee_identity = local_identity_;
  pending.media_mode = CallMediaMode::Voice;
  pending.origin_thread_id = "thread:2";
  pending.expires_at = util::NowUnixMs() - 1;
  pending.created_at = util::NowUnixMs() - 60'000;
  pending.status = "pending";
  ASSERT_TRUE(sessions_->UpsertPendingInvite(pending));

  CallSession session;
  session.call_id = pending.call_id;
  session.origin_thread_id = "thread:2";
  session.media_mode = CallMediaMode::Voice;
  session.state = CallSessionState::Ringing;
  session.created_at = pending.created_at;
  ASSERT_TRUE(sessions_->UpsertSession(session));

  device_->LiveCallsForTest().AdmitInvited(pending.call_id, {"account:peer"}); // as the inbound invite does
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, pending.call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::Ringing);
  ASSERT_TRUE((device_->Live().Phase() != CallPhase::Idle));

  OnCallsOwner([&] { return csm_->SweepExpiredInvites(); });

  auto top = reads_->TopPendingInvite();
  ASSERT_TRUE(top);
  EXPECT_FALSE(top->has_value());
  auto self = sessions_->FindParticipant(pending.call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Missed);
  auto loaded = sessions_->LoadSession(pending.call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle)
      << "CALLS expire → Idle (no DeclineClicked); got " << CallPhaseName(device_->Live().Phase());
  EXPECT_FALSE((device_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachIgnoredWhileDirectConnecting) {
  const std::string call_id = "call:sfu-ignored";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  DrainUntil([&]() { return device_->Live().MediaAttempted(call_id) || media_->IsActive(); });
  ASSERT_TRUE(device_->Live().AllowsDirectPath());

  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = "12D3KooWHop";
  attach.hop_multiaddr = "/ip4/10.0.0.9/tcp/4001/p2p/12D3KooWHop";
  attach.quote_id = "q:1";
  attach.publisher_stream_id = 1;
  auto detail = CallControlCodec::EncodeSfuAttach(attach);
  ASSERT_TRUE(detail);
  auto attach_msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallSfuAttach, "SFU", *detail,
                                                         "account:peer");
  ASSERT_TRUE(attach_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*attach_msg, "account:peer"); }));

  // V037/V038: DirectConnecting Status must not flip to hop attach.
  EXPECT_TRUE(device_->Live().AllowsDirectPath());
  EXPECT_FALSE(device_->Live().AllowsHopPath());
  EXPECT_FALSE(device_->IsSfuAttached());
}

TEST_F(CallSessionInboundComposeTest, InboundHopRefuseEndsBoundCall) {
  const std::string call_id = "call:hop-refuse";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  DrainUntil([&]() { return device_->Seat().IsBound(call_id) || media_->IsActive(); });
  ASSERT_TRUE(device_->Seat().IsBound(call_id) || media_->IsActive());

  CallHopRefuseDetail refuse;
  refuse.call_id = call_id;
  refuse.identity = local_identity_;
  refuse.reason = "no_shared_hop";
  refuse.message = "No shared hop";
  auto detail = CallControlCodec::EncodeHopRefuse(refuse);
  ASSERT_TRUE(detail);
  auto refuse_msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallHopRefuse, "Refuse",
                                                         *detail, "account:peer");
  ASSERT_TRUE(refuse_msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*refuse_msg, "account:peer"); }));

  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  auto err = device_->TakeLastMediaError();
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(err->kind, CallMediaErrorKind::Other);
  EXPECT_EQ(err->message, "No shared hop");
}

// A control type this build does not know (removed, or from a newer peer) is ignored cleanly: no error, no call.
TEST_F(CallSessionInboundComposeTest, AnUnknownControlTypeIsIgnored) {
  auto known = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallInvite, "x",
                                                    R"({"call_id":"call:x"})", "account:peer");
  ASSERT_TRUE(known);
  for (const char* type : {"call_sdp", "call_ice", "call_join", "call_from_the_future"}) {
    ThreadMessage unknown = *known;
    Object payload;
    payload.set("control_type", type);
    payload.set("detail", R"({"call_id":"call:x"})");
    unknown.payload_json = DumpJson(payload);
    EXPECT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(unknown, "account:peer"); })) << type;
  }

  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, DeclineClickedClearsPendingViaLifecycle) {
  const std::string call_id = "call:decline-life";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Ringing);

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::DeclineClicked, call_id); });
  DrainUntil([&]() {
    auto pending = reads_->TopPendingInvite();
    return pending && !pending->has_value() && device_->Live().Phase() == CallPhase::Idle;
  });
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
  auto pending = reads_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, InboundDeclineClearsOffererOutboundCalling) {
  // CALLS: peer Decline → offerer Idle (no local LeaveClicked / TTL wait).
  const std::string call_id = "call:inbound-decline";
  SeedOffererRingingCall(call_id);
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);
  ASSERT_TRUE(reads_->ActiveLocalCall()->has_value());

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = "account:peer";
  auto detail = CallControlCodec::EncodeDecline(decline);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "inbound CallDecline must EndCallLocal → RemoteEnded";
  EXPECT_TRUE(ShownCallId().empty());
  auto active = reads_->ActiveLocalCall();
  ASSERT_TRUE(active);
  EXPECT_FALSE(active->has_value());
}

TEST_F(CallSessionInboundComposeTest, InboundDeclineKeepsCallWhenOtherInviteeStillRinging) {
  // Group-shaped: one Decline must not end while another remote still rings.
  const std::string call_id = "call:decline-keep";
  SeedOffererRingingCall(call_id);
  CallParticipant other;
  other.call_id = call_id;
  other.identity = "account:other";
  other.state = CallParticipantState::Ringing;
  ASSERT_TRUE(sessions_->UpsertParticipant(other));

  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = "account:peer";
  auto detail = CallControlCodec::EncodeDecline(decline);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_NE((*loaded)->state, CallSessionState::Ended)
      << "first Decline must keep session while account:other still Ringing";
  EXPECT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling)
      << "got phase=" << CallPhaseName(device_->Live().Phase());
  EXPECT_EQ(ShownCallId(), call_id);

  CallDeclineDetail decline_other;
  decline_other.call_id = call_id;
  decline_other.identity = "account:other";
  auto detail_other = CallControlCodec::EncodeDecline(decline_other);
  ASSERT_TRUE(detail_other);
  auto msg_other = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined",
                                                        *detail_other, "account:other");
  ASSERT_TRUE(msg_other);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg_other, "account:other"); }));

  loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
}

TEST_F(CallSessionInboundComposeTest, InboundVideoRefreshHonoredOnlyWhenJoinedActive) {
  const std::string call_id = "call:vref";
  // Wrong call / not joined → no-op success.
  CallVideoRefreshDetail refresh;
  refresh.call_id = call_id;
  refresh.identity = local_identity_;
  auto detail = CallControlCodec::EncodeVideoRefresh(refresh);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallVideoRefresh, "IDR", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto invite = MakeInviteMessage(call_id);
  ASSERT_TRUE(invite);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*invite, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  DrainUntil([&]() { return media_->IsActive() || device_->Live().MediaAttempted(call_id); });

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachFailedDispatched) {
  const std::string call_id = "call:sfu-fail";
  SeedOffererRingingCall(call_id);
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });

  CallSfuAttachFailedDetail fail;
  fail.call_id = call_id;
  fail.identity = "account:peer";
  fail.failed_hop_peer_id = "12D3KooWHop";
  fail.error = "attach_failed";
  auto detail = CallControlCodec::EncodeSfuAttachFailed(fail);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallSfuAttachFailed, "fail", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
}

TEST_F(CallSessionInboundComposeTest, RetryP2pMediaAfterConnectFailed) {
  const std::string call_id = "call:retry";
  SeedOffererRingingCall(call_id);
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  device_->LiveCallsForTest().SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, "test");

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  DrainUntil([&]() { return device_->Live().MediaAttempted(call_id) || media_->IsActive(); });
  ASSERT_TRUE(device_->Live().MediaAttempted(call_id));

  OnCallsOwner([&] { device_->LiveCallsForTest().NoteMediaFailed(call_id); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::ConnectFailed);
  EXPECT_FALSE(device_->Live().AllowsDirectPath());

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::RetryClicked, call_id); });
  EXPECT_TRUE(device_->Live().AllowsDirectPath()) << "RetryClicked re-arms DirectConnecting";
  DrainUntil([&]() {
    return device_->Live().Phase() == CallPhase::MediaConnecting || device_->Live().Phase() == CallPhase::InCall;
  });
  EXPECT_TRUE(device_->Live().Phase() == CallPhase::MediaConnecting || device_->Live().Phase() == CallPhase::InCall)
      << "phase=" << CallPhaseName(device_->Live().Phase()) << " err=" << device_->LastError();

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id); });
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, StartCallOutboundCreatesSessionAndInvite) {
  Thread thread;
  // Windows: thread id is a directory name under threads/ — no ':' (illegal path char).
  thread.id = "thread-dm-out";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer"}); });
  ASSERT_TRUE(started) << started.error().message;
  EXPECT_FALSE(started->call_id.empty());
  EXPECT_EQ(started->state, CallSessionState::Ringing);
  EXPECT_EQ(started->origin_thread_id, thread.id);

  auto self = sessions_->FindParticipant(started->call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
  EXPECT_GE(sent_control_messages_, 1);
  EXPECT_EQ(invite_urgent_, std::vector<bool>{true}) << "an ordinary invite wakes the peer";
  auto key = keys_->LoadEpochKey(started->call_id, 1);
  ASSERT_TRUE(key && key->has_value());
}

/** The `detail` of a sent call-control message's payload. */
std::string DetailOf(const std::string& payload_json) {
  auto payload = TryParseObject(payload_json);
  return payload ? payload->getString("detail").value_or("") : std::string();
}

// M024: an invite carries the media key itself — its lane copy is sealed for one device of the callee's.
TEST_F(CallSessionInboundComposeTest, AnInviteCarriesTheKey) {
  Thread thread;
  thread.id = "thread-dm-key-forms";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));
  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer"}); });
  ASSERT_TRUE(started) << started.error().message;
  auto key = keys_->LoadEpochKey(started->call_id, 1);
  ASSERT_TRUE(key && key->has_value());

  auto invite = CallControlCodec::DecodeInvite(DetailOf(last_sent_invite_payload_));
  ASSERT_TRUE(invite) << invite.error().message;
  EXPECT_EQ(Base64Decode(invite->media_key_b64).value(), **key) << "the key itself";
}

// M024: a device that reads its own lane copy takes the key it carries — no session of the calls' own, so a callee
// whose account session went another device's way (B186) still has the key.
TEST_F(CallSessionInboundComposeTest, AnInvitesKeyIsTakenFromItsLaneCopy) {
  const std::string call_id = "call:key-in-copy";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  auto payload = TryParseObject(msg->payload_json);
  ASSERT_TRUE(payload);
  auto invite = CallControlCodec::DecodeInvite(payload->getString("detail").value_or(""));
  ASSERT_TRUE(invite);
  invite->media_key_b64 = Base64Encode(TestMediaKey());
  auto detail = CallControlCodec::EncodeInvite(*invite);
  ASSERT_TRUE(detail);
  auto with_key = CallControlCodec::BuildSystemMessage("", CallControlType::CallInvite, "Incoming voice call", *detail,
                                                       "account:peer");
  ASSERT_TRUE(with_key);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*with_key, "account:peer"); }));
  auto stored = keys_->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(stored && stored->has_value()) << "taken from the invite";
  EXPECT_EQ(**stored, TestMediaKey());
}

// The joiner answers this invite on its own, so it must not ring (or push) the joiner's device.
TEST_F(CallSessionInboundComposeTest, InviteAnsweringAJoinRequestIsNotUrgent) {
  Thread thread;
  thread.id = "thread-dm-join";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));
  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer"}); });
  ASSERT_TRUE(started) << started.error().message;
  ASSERT_EQ(invite_urgent_.size(), 1u);

  CallJoinRequestDetail request;
  request.call_id = started->call_id;
  request.identity = "account:peer";
  auto detail = CallControlCodec::EncodeJoinRequest(request);
  ASSERT_TRUE(detail) << detail.error().message;
  auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallJoinRequest, "Joining the call",
                                                  *detail, "account:peer");
  ASSERT_TRUE(msg) << msg.error().message;
  auto applied = OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); });
  ASSERT_TRUE(applied) << applied.error().message;
  ASSERT_EQ(invite_urgent_.size(), 2u) << "the join request was not answered with an invite";
  EXPECT_FALSE(invite_urgent_[1]);
}

// PR #240 review: a new call whose invites cannot go out must leave the current call alone, and must
// not stay behind ringing.
TEST_F(CallSessionInboundComposeTest, StartCallThatCannotInviteKeepsTheCurrentCall) {
  for (const char* id : {"thread-dm-a", "thread-dm-b"}) {
    Thread thread;
    thread.id = id;
    thread.kind = ThreadKind::Direct;
    thread.title = "Peer";
    thread.updated_at = util::NowUnixMs();
    ASSERT_TRUE(store_->UpsertThread(thread));
  }
  auto a = OnCallsOwner([&] { return csm_->StartCall("thread-dm-a", false, {"account:peer"}); });
  ASSERT_TRUE(a) << a.error().message;

  fail_sends_ = true;
  auto b = OnCallsOwner([&] { return csm_->StartCall("thread-dm-b", false, {"account:other"}); });
  ASSERT_FALSE(b) << "the invite could not be sent";

  const LiveCall* active = device_->Live().Active();
  ASSERT_NE(active, nullptr);
  EXPECT_EQ(active->Id(), a->call_id) << "the current call is still the one this device is in";
  auto a_row = sessions_->LoadSession(a->call_id);
  ASSERT_TRUE(a_row && a_row->has_value());
  EXPECT_NE((*a_row)->state, CallSessionState::Ended);

  auto sessions = sessions_->ListActiveSessions();
  ASSERT_TRUE(sessions);
  for (const CallSession& row : *sessions) {
    EXPECT_EQ(row.call_id, a->call_id) << "the failed call was left behind: " << row.call_id;
  }
}

// V058 2: a group call rings whoever can be invited; one invitee out of reach does not stop the start.
TEST_F(CallSessionInboundComposeTest, AGroupCallRingsWhoeverCanBeInvited) {
  Thread thread;
  thread.id = "thread-group-out";
  thread.kind = ThreadKind::Group;
  thread.title = "Group";
  thread.group_id = "group-out";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));
  unreachable_peer_ = "account:away";

  auto started = OnCallsOwner(
      [&] { return csm_->StartCall(thread.id, false, {"account:away", "account:peer1", "account:peer2"}); });
  ASSERT_TRUE(started) << started.error().message;
  EXPECT_EQ(started->state, CallSessionState::Ringing);
  EXPECT_GE(sent_control_messages_, 2) << "the two in reach were invited";
  ASSERT_NE(device_->Live().Active(), nullptr);
  EXPECT_EQ(device_->Live().Active()->Id(), started->call_id);
}

// The start fails only when nobody could be invited.
TEST_F(CallSessionInboundComposeTest, AGroupCallNobodyCanBeInvitedToDoesNotStart) {
  Thread thread;
  thread.id = "thread-group-none";
  thread.kind = ThreadKind::Group;
  thread.title = "Group";
  thread.group_id = "group-none";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));
  fail_sends_ = true;

  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer1", "account:peer2"}); });
  EXPECT_FALSE(started);
  EXPECT_EQ(device_->Live().Active(), nullptr);
}

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesAutoLeavesOutboundUnanswered) {
  // CALLS: OutboundCalling + no media past TTL → Leave via Sweep (not GUI LeaveClicked).
  Thread thread;
  thread.id = "thread-dm-ttl";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer"}); });
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);

  // Age the session past invite TTL without waiting 60s.
  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  (*session)->created_at = util::NowUnixMs() - kDefaultCallInviteTtlMs - 1;
  ASSERT_TRUE(sessions_->UpsertSession(**session));

  OnCallsOwner([&] { return csm_->SweepExpiredInvites(); });
  DrainUntil([&]() { return device_->Live().Phase() == CallPhase::Idle && !reads_->ActiveLocalCall()->has_value(); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_FALSE((device_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesSkipsOutboundBeforeTtl) {
  Thread thread;
  thread.id = "thread-dm-ttl-early";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = OnCallsOwner([&] { return csm_->StartCall(thread.id, false, {"account:peer"}); });
  ASSERT_TRUE(started) << started.error().message;
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(started->call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);

  OnCallsOwner([&] { return csm_->SweepExpiredInvites(); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);
  EXPECT_TRUE(reads_->ActiveLocalCall()->has_value());
}

TEST_F(CallSessionInboundComposeTest, MuteAndVideoControlsOnActiveMedia) {
  const std::string call_id = "call:mute-video";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  // Allow video for enable gate (invite default voice → flip session flag).
  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  (*session)->video_allowed = true;
  ASSERT_TRUE(sessions_->UpsertSession(**session));

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::InviteSeen, call_id); });
  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id); });
  DrainUntil([&]() { return media_->IsActive(); });
  ASSERT_TRUE(media_->IsActive());
  EXPECT_TRUE(device_->Live().MediaAttempted(call_id));

  auto peer = reads_->PeerIdentityForCall(call_id);
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ(**peer, "account:peer");
  auto video_ok = reads_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(video_ok && video_ok->has_value());
  EXPECT_TRUE(**video_ok);

  const int sent_before = sent_control_messages_;
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalAudioMuted(true); }));
  EXPECT_TRUE(media_->IsMuted());
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_TRUE((*self)->media.audio_muted);
  EXPECT_GT(sent_control_messages_, sent_before) << "mute should fan-out CallRoster";

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalAudioMuted(false); }));
  EXPECT_FALSE(media_->IsMuted());

  // The OS took the call's audio (a phone call): the others are told it is on hold here, then that it is back.
  const int before_hold = sent_control_messages_;
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalOnHold(true); }));
  EXPECT_TRUE(sessions_->FindParticipant(call_id, local_identity_).value()->media.on_hold);
  EXPECT_GT(sent_control_messages_, before_hold) << "hold should fan-out CallRoster";
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalOnHold(false); }));
  EXPECT_FALSE(sessions_->FindParticipant(call_id, local_identity_).value()->media.on_hold);
  // No camera on: the background pauses nothing (and says nothing).
  const int before_pause = sent_control_messages_;
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalVideoPaused(true, 0); }));
  EXPECT_FALSE(sessions_->FindParticipant(call_id, local_identity_).value()->media.video_paused);
  EXPECT_EQ(sent_control_messages_, before_pause);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalVideoPaused(false, 0); }));

  // Camera may fail headless — gate must still accept video_allowed before device open.
  auto enable = OnCallsOwner([&] { return csm_->SetLocalVideoEnabled(true, 0); });
  if (enable) {
    EXPECT_TRUE(media_->IsCameraEnabled());
    ASSERT_TRUE(OnCallsOwner([&] { return csm_->SetLocalVideoEnabled(false, 0); }));
  } else {
    EXPECT_FALSE(enable.error().message.empty());
  }

  // Voice-only session rejects enable.
  auto voice_only = sessions_->LoadSession(call_id);
  ASSERT_TRUE(voice_only && voice_only->has_value());
  (*voice_only)->video_allowed = false;
  ASSERT_TRUE(sessions_->UpsertSession(**voice_only));
  auto denied = OnCallsOwner([&] { return csm_->SetLocalVideoEnabled(true, 0); });
  EXPECT_FALSE(denied);
  EXPECT_NE(denied.error().message.find("Video is not allowed"), std::string::npos);

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->RequestVideoRefresh(call_id, local_identity_); }));
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->RequestVideoRefresh(call_id, {}); }));

  OnCallsOwner([&] { return csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id); });
  DrainUntil([&]() {
    auto row = sessions_->LoadSession(call_id);
    return row && row->has_value() && (*row)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, MuteWithoutActiveMediaFails) {
  auto muted = OnCallsOwner([&] { return csm_->SetLocalAudioMuted(true); });
  EXPECT_FALSE(muted);
  EXPECT_NE(muted.error().message.find("No active call media"), std::string::npos);
}

/** V062: a group's call on a device that checks guests against the group's roster. */
class CallGuestComposeTest : public CallSessionInboundComposeTest {
protected:
  void SetUp() override {
    with_group_standing_ = true;
    CallSessionInboundComposeTest::SetUp();
  }

  static CallGroupStanding OwnerStanding() {
    return {.member = true, .may_bring_guest = true, .may_remove_guest = true, .may_invite_to_group = true};
  }
  static CallGroupStanding MemberStanding(const bool may_bring_guest) {
    return {.member = true, .may_bring_guest = may_bring_guest};
  }

  /** Group call `call_id` of group:g, on the hop: us, the owner (account:peer), Bob and his guest Erin, all joined. */
  void SeedGroupCallWithGuest(const std::string& call_id) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread:g";
    session.origin_group_id = "group:g";
    session.state = CallSessionState::Active;
    session.created_at = util::NowUnixMs();
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    session.sfu_hint = "12D3KooWHop";
    ASSERT_TRUE(sessions_->UpsertSession(session));
    int64_t joined_at = session.created_at;
    for (const std::string& identity :
         {local_identity_, std::string("account:peer"), std::string("account:bob"), std::string("account:erin")}) {
      CallParticipant row;
      row.call_id = call_id;
      row.identity = identity;
      row.state = CallParticipantState::Joined;
      row.joined_at = joined_at++;
      row.brought_by = identity == "account:erin" ? "account:bob" : "";
      ASSERT_TRUE(sessions_->UpsertParticipant(row));
    }
    ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
    standings_["account:peer"] = OwnerStanding();
    standings_["account:bob"] = MemberStanding(true);
    standings_[local_identity_] = MemberStanding(true);
  }

  Roe<void> ReceiveRemove(const std::string& call_id, const std::string& who, const std::string& from) {
    auto detail = CallControlCodec::EncodeRemove({call_id, who});
    if (!detail) {
      return detail.error();
    }
    auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallRemove, "", *detail, from);
    if (!msg) {
      return msg.error();
    }
    return OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, from); });
  }

  CallParticipantState StateOf(const std::string& call_id, const std::string& identity) {
    auto row = sessions_->FindParticipant(call_id, identity);
    return row && row->has_value() ? (*row)->state : CallParticipantState::Invited;
  }
};

// V062 3: a plain member's call_remove is refused; the owner's takes the guest out (her leave: the key rotates).
TEST_F(CallGuestComposeTest, TheOwnerRemovesAGuestAPlainMemberCannot) {
  const std::string call_id = "call:guest-remove";
  SeedGroupCallWithGuest(call_id);

  ASSERT_TRUE(ReceiveRemove(call_id, "account:erin", "account:bob"));
  EXPECT_EQ(StateOf(call_id, "account:erin"), CallParticipantState::Joined) << "a plain member removes nobody";

  ASSERT_TRUE(ReceiveRemove(call_id, "account:erin", "account:peer"));
  EXPECT_EQ(StateOf(call_id, "account:erin"), CallParticipantState::Left);
}

// V062 3: members are not removed from a call this way, not even by the owner.
TEST_F(CallGuestComposeTest, CallRemoveNamingAMemberIsRefused) {
  const std::string call_id = "call:guest-remove-member";
  SeedGroupCallWithGuest(call_id);

  ASSERT_TRUE(ReceiveRemove(call_id, "account:bob", "account:peer"));
  EXPECT_EQ(StateOf(call_id, "account:bob"), CallParticipantState::Joined);
  ASSERT_TRUE(ReceiveRemove(call_id, local_identity_, "account:peer"));
  EXPECT_EQ(StateOf(call_id, local_identity_), CallParticipantState::Joined) << "we are a member: we stay";
}

// V062 3: the guest's own device holds no roster: removed, it leaves the call.
TEST_F(CallGuestComposeTest, ARemovedGuestsDeviceLeaves) {
  const std::string call_id = "call:guest-leaves";
  SeedGroupCallWithGuest(call_id);
  holds_roster_ = false;

  ASSERT_TRUE(ReceiveRemove(call_id, local_identity_, "account:peer"));
  EXPECT_EQ(StateOf(call_id, local_identity_), CallParticipantState::Left);
}

// V062 2: a roster naming a guest is taken only from a bringer the group's preset lets bring one.
TEST_F(CallGuestComposeTest, ARosterGuestFromABringerNotAllowedIsDropped) {
  const std::string call_id = "call:guest-roster";
  SeedGroupCallWithGuest(call_id);
  standings_["account:bob"] = MemberStanding(false); // a Managed group: Bob may not bring guests

  CallRosterDetail roster;
  roster.call_id = call_id;
  CallRosterEntry frank;
  frank.identity = "account:frank";
  frank.brought_by = "account:bob";
  CallRosterEntry gina;
  gina.identity = "account:gina";
  gina.brought_by = "account:peer";
  roster.participants = {frank, gina};
  auto detail = CallControlCodec::EncodeRoster(roster);
  ASSERT_TRUE(detail);
  auto msg =
      CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallRoster, "", *detail, "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));

  auto dropped = sessions_->FindParticipant(call_id, "account:frank");
  ASSERT_TRUE(dropped);
  EXPECT_FALSE(dropped->has_value()) << "brought by a member who may not";
  auto kept = sessions_->FindParticipant(call_id, "account:gina");
  ASSERT_TRUE(kept && kept->has_value());
  EXPECT_EQ((*kept)->brought_by, "account:peer");
}

// V062 2: inviting someone outside the group: a guest invite from a member allowed to; refused from one who is not.
TEST_F(CallGuestComposeTest, AMidCallInviteOfAStrangerIsAGuestInvite) {
  const std::string call_id = "call:guest-invite";
  SeedGroupCallWithGuest(call_id);

  ASSERT_TRUE(OnCallsOwner([&] { return csm_->InviteParticipant(call_id, "account:hal"); }));
  auto payload = TryParseObject(last_sent_payload_);
  ASSERT_TRUE(payload);
  auto invite = CallControlCodec::DecodeInvite(payload->getString("detail").value_or(""));
  ASSERT_TRUE(invite) << last_sent_payload_;
  EXPECT_TRUE(invite->guest);
  EXPECT_EQ(invite->brought_by, local_identity_);
  auto row = sessions_->FindParticipant(call_id, "account:hal");
  ASSERT_TRUE(row && row->has_value());
  EXPECT_EQ((*row)->brought_by, local_identity_);

  standings_[local_identity_] = MemberStanding(false); // a Managed group's plain member
  EXPECT_FALSE(OnCallsOwner([&] { return csm_->InviteParticipant(call_id, "account:ivy"); }));
  standings_["account:jo"] = MemberStanding(false);
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->InviteParticipant(call_id, "account:jo"); })) << "a member: plain";
  auto plain = CallControlCodec::DecodeInvite(TryParseObject(last_sent_payload_)->getString("detail").value_or(""));
  ASSERT_TRUE(plain);
  EXPECT_FALSE(plain->guest);
}

// V062 3: removing from this device: only the owner and admins, only a guest.
TEST_F(CallGuestComposeTest, RemovingFromHereNeedsOwnerOrAdminAndAGuest) {
  const std::string call_id = "call:guest-remove-local";
  SeedGroupCallWithGuest(call_id);

  EXPECT_FALSE(OnCallsOwner([&] { return csm_->RemoveFromCall(call_id, "account:erin"); })) << "a plain member";
  standings_[local_identity_] = OwnerStanding();
  EXPECT_FALSE(OnCallsOwner([&] { return csm_->RemoveFromCall(call_id, "account:bob"); })) << "a member";
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->RemoveFromCall(call_id, "account:erin"); }));
  EXPECT_EQ(StateOf(call_id, "account:erin"), CallParticipantState::Left);
}

// DEV/PLAN-call-ui-never-detached.md reviews 3 and 4 (B199): a read of the call's people that fails never ends a call
// that may go on, and never keeps one that is certainly over. A 1:1 call needs no count: its one peer leaving or
// declining, or this device leaving, is the end. A group's call does what it can now and decides "is anyone left" once
// readable — or from the live call after a few failed reads.
TEST_F(CallSessionInboundComposeTest, AOneToOnePeerLeavingEndsTheCallWhenTheRosterCannotBeRead) {
  const std::string call_id = "call:leave-unread";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/false));
  const size_t summaries_before = summaries_.size();
  sessions_->fail_participant_lists = true; // the sender check reads; the roster read fails
  ASSERT_TRUE(PeerLeaves(call_id, "account:peer"));
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "a 1:1 call stayed open after its peer left";
  EXPECT_EQ(SessionState(call_id), CallSessionState::Ended);
  EXPECT_FALSE(media_->IsActive());
  EXPECT_GT(summaries_.size(), summaries_before) << "the call's line in the chat";
}

TEST_F(CallSessionInboundComposeTest, AOneToOneDeclineEndsTheCallWhenTheRosterCannotBeRead) {
  const std::string call_id = "call:decline-unread";
  SeedOffererRingingCall(call_id);
  OnCallsOwner([&] { device_->LiveCallsForTest().NoteOutboundStarted(call_id); });
  ASSERT_EQ(device_->Live().Phase(), CallPhase::OutboundCalling);
  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = "account:peer";
  auto detail = CallControlCodec::EncodeDecline(decline);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  sessions_->fail_participant_lists = true;
  ASSERT_TRUE(OnCallsOwner([&] { return csm_->ApplyInboundControl(*msg, "account:peer"); }));
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "the only one asked said no";
  EXPECT_EQ(SessionState(call_id), CallSessionState::Ended);
}

TEST_F(CallSessionInboundComposeTest, OurOwnOneToOneLeaveEndsTheCallWhenTheRosterCannotBeRead) {
  const std::string call_id = "call:own-leave-unread";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/false));
  const size_t summaries_before = summaries_.size();
  sessions_->fail_participants = true;
  (void)OnCallsOwner([&] { return csm_->LeaveCall(call_id, LiveCallEndReason::LocalLeave); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle);
  EXPECT_FALSE(media_->IsActive());
  EXPECT_EQ(SessionState(call_id), CallSessionState::Ended) << "a 1:1 call ends when this device leaves";
  EXPECT_GT(summaries_.size(), summaries_before) << "the final line in the chat";
}

TEST_F(CallSessionInboundComposeTest, OurOwnGroupLeaveCompletesButEndsItForNoOneWhenTheRosterCannotBeRead) {
  const std::string call_id = "call:own-group-leave-unread";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/true, {"account:peer", "account:peer2"}));
  sessions_->fail_participants = true;
  const int ended_before = sent_ended_;
  (void)OnCallsOwner([&] { return csm_->LeaveCall(call_id, LiveCallEndReason::LocalLeave); });
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "this device is out whatever the read said";
  EXPECT_FALSE(media_->IsActive());
  EXPECT_EQ(sent_ended_, ended_before) << "no call_ended on a failed read";
  EXPECT_EQ(SessionState(call_id), CallSessionState::Active) << "a group's call goes on for the others";
  auto line = store_->GetMessage("thread:1", GroupCallSummaryMessageId(call_id));
  EXPECT_TRUE(line && line->has_value()) << "this device's line, as on any group leave";
  sessions_->fail_participants = false;
  auto me = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(me && me->has_value());
  EXPECT_EQ((*me)->state, CallParticipantState::Left);
}

TEST_F(CallSessionInboundComposeTest, AGroupPeerLeavingWithAnUnreadableRosterIsDecidedOnceReadable) {
  const std::string call_id = "call:group-leave-unread";
  // Ids that sort after this device's: it is the key's coordinator (the lowest id among those left).
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/true, {"account:~leaver", "account:~stays"}));
  OnCallsOwner([&] { device_->Workflow().SetUnreadRosterRetryForTest(0); });
  sessions_->fail_participant_lists = true;
  ASSERT_TRUE(PeerLeaves(call_id, "account:~leaver"));
  EXPECT_EQ(LivePeers(call_id), std::vector<std::string>{"account:~stays"}) << "out of the live call at once";
  EXPECT_EQ(device_->Live().Phase(), CallPhase::InCall) << "a group's call goes on";
  EXPECT_EQ(SessionState(call_id), CallSessionState::Active);

  sessions_->fail_participant_lists = false; // readable again: the next sweep decides
  ASSERT_LT(local_identity_, std::string("account:~stays")) << "this device coordinates the key in this test";
  Sweep();
  EXPECT_EQ(MediaEpoch(call_id), 2u) << "the one who left keeps no working key";
  EXPECT_EQ(device_->Live().Phase(), CallPhase::InCall) << "~stays is still in it";
  EXPECT_EQ(device_->AloneUntilMs(call_id), 0) << "not alone";
}

TEST_F(CallSessionInboundComposeTest, AGroupEveryoneLeftWithARosterReadableAgainWaitsAloneThenLeaves) {
  const std::string call_id = "call:group-all-left";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/true));
  OnCallsOwner([&] { device_->Workflow().SetUnreadRosterRetryForTest(0); });
  sessions_->fail_participant_lists = true;
  ASSERT_TRUE(PeerLeaves(call_id, "account:peer"));
  EXPECT_EQ(device_->Live().Phase(), CallPhase::InCall);
  sessions_->fail_participant_lists = false;
  Sweep();
  EXPECT_GT(device_->AloneUntilMs(call_id), 0) << "nobody else: the alone wait (V053) starts";
  OnCallsOwner([&] {
    device_->Workflow().SetAloneSinceForTest(call_id, util::NowUnixMs() - CallSessionLogic::kAloneInGroupCallMs);
  });
  Sweep();
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "alone for the whole wait: left";
}

TEST_F(CallSessionInboundComposeTest, AGroupEveryoneLeftWithARosterThatNeverReadsEndsFromTheLiveCall) {
  const std::string call_id = "call:group-never-reads";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/true));
  OnCallsOwner([&] { device_->Workflow().SetUnreadRosterRetryForTest(0); });
  sessions_->fail_participant_lists = true;
  ASSERT_TRUE(PeerLeaves(call_id, "account:peer"));
  for (int i = 0; i < 6; ++i) { // past the bounded reads: decided from the live call (no peers left in it)
    Sweep();
  }
  EXPECT_GT(device_->AloneUntilMs(call_id), 0) << "the live call has nobody else: the alone wait starts";
  OnCallsOwner([&] {
    device_->Workflow().SetAloneSinceForTest(call_id, util::NowUnixMs() - CallSessionLogic::kAloneInGroupCallMs);
  });
  for (int i = 0; i < 6 && device_->Live().Phase() != CallPhase::Idle; ++i) {
    Sweep();
  }
  EXPECT_EQ(device_->Live().Phase(), CallPhase::Idle) << "never readable: left once the live call had nobody";
  EXPECT_FALSE(media_->IsActive());
}

TEST_F(CallSessionInboundComposeTest, AloneInAGroupCallWithAnUnreadableRosterIsAskedAgainNotLeft) {
  const std::string call_id = "call:alone-unread";
  ASSERT_NO_FATAL_FAILURE(SeedConnectedCall(call_id, /*group=*/true));
  const int64_t long_ago = util::NowUnixMs() - CallSessionLogic::kAloneInGroupCallMs - 1'000;
  OnCallsOwner([&] { device_->Workflow().SetAloneSinceForTest(call_id, long_ago); });
  sessions_->fail_participants = true;
  Sweep();
  EXPECT_EQ(device_->Live().Phase(), CallPhase::InCall) << "left on one failed read (the live call still has the peer)";
  EXPECT_GT(device_->AloneUntilMs(call_id), 0) << "still waited on: asked again";
}

} // namespace
} // namespace pbr
