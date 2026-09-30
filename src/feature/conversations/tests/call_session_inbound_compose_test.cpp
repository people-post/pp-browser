#include "domain/messaging/CallLifecycleTypes.h"
#include "feature/conversations/tests/calls_loopback_outbox.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallTopologyController.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/ValueJson.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "foundation/crypto/SessionKeyDeriver.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <atomic>
#include <optional>
#include "feature/conversations/tests/call_media_inbound_fake.h"

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

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

class MemoryPskStore final : public IPskSessionStore {
public:
  void SeedMasterPsk(ByteVector master_psk) {
    master_psk_ = std::move(master_psk);
    master_psk_b64_ = Base64Encode(*master_psk_);
  }

  Roe<std::optional<PskSessionRecord>> Load(const ChatTargetKey& /*key*/) const override {
    if (!master_psk_) {
      return std::optional<PskSessionRecord>{};
    }
    PskSessionRecord record;
    record.session_epoch = 1;
    record.master_psk_b64 = master_psk_b64_;
    return std::optional<PskSessionRecord>{std::move(record)};
  }
  Roe<std::vector<PskSessionRecord>> List() const override { return std::vector<PskSessionRecord>{}; }
  Roe<void> Save(const PskSessionRecord& /*record*/) override { return {}; }
  Roe<ByteVector> GenerateMasterPsk() override { return ByteVector(32, 0x11); }
  Roe<std::optional<std::string>> ResolveMasterPskForEpoch(const ChatTargetKey& /*key*/,
                                                           uint32_t /*epoch*/) const override {
    if (!master_psk_) {
      return std::optional<std::string>{};
    }
    return std::optional<std::string>{master_psk_b64_};
  }
  Roe<void> MarkPskVerified(const ChatTargetKey& /*key*/, int64_t /*verified_at_ms*/) override { return {}; }
  Roe<bool> IsPskVerified(const ChatTargetKey& /*key*/) const override { return false; }
  Roe<PskBundleV1> ExportPskBundle(const ChatTargetKey& /*key*/) const override { return PskBundleV1{}; }
  Roe<void> ImportPskBundle(const ChatTargetKey& /*key*/, const PskBundleV1& /*bundle*/) override { return {}; }

private:
  std::optional<ByteVector> master_psk_;
  std::string master_psk_b64_;
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
  void Detach() override {
    active = false;
    ++detach_calls;
  }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(Roe<void>)> on_done, int /*timeout_ms*/) override {
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
  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    int timeout_ms) override {
    Roe<void> out;
    ConnectAsync(params, std::move(callbacks), [&](Roe<void> r) { out = std::move(r); }, timeout_ms);
    return out;
  }
  Roe<void> SendAudio(const std::vector<uint8_t>& /*opus*/, uint32_t /*seq*/, uint8_t /*mark*/) override {
    return {};
  }
  Roe<void> SendMedia(uint8_t /*channel*/, const std::vector<uint8_t>& /*payload*/, uint32_t /*seq*/,
                      uint8_t /*mark*/) override {
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

CallDirectMediaPorts TestDirectMediaPorts(CallMediaBridge* bridge, CallMediaSeat* seat) {
  CallDirectMediaPorts ports;
  if (!bridge) {
    return ports;
  }
  ports.media_path_kind = [bridge]() { return bridge->MediaPathKind(); };
  ports.note_peer_id_relay_mapping = [bridge](const std::string& peer_id,
                                              const std::string& relay_identity) {
    bridge->NotePeerIdRelayMapping(peer_id, relay_identity);
  };
  ports.is_connect_failed = [bridge]() { return bridge->IsMeshConnectFailed(); };
  ports.connect_missing_mic = [bridge]() {
    return bridge->IsMeshConnectFailed() && bridge->MeshConnectMissingMic();
  };
  ports.poll_connect_health = [bridge]() { bridge->PollMeshConnectHealth(); };
  ports.media_attempted = [bridge](const std::string& call_id) {
    return bridge->MediaAttempted(call_id);
  };
  ports.note_media_attempted = [bridge](const std::string& call_id) {
    bridge->NoteMediaAttempted(call_id);
  };
  ports.on_media_key_ready = [bridge](const std::string& call_id) {
    bridge->OnMediaKeyReady(call_id);
  };
  return ports;
}

class CallSessionInboundComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(ManualOwnerRuntimeConfig());
    AppRuntime::InitializeUI();

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_csm_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);

    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    sessions_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
    keys_ = std::make_unique<CallMediaKeyStore>(store_->ProfileDbPath());
    ASSERT_TRUE(keys_->SetDek(TestDek()));
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "csm-test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    auto loaded = identity_->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    local_identity_ = loaded->account_id;
    ASSERT_FALSE(local_identity_.empty());

    psk_ = std::make_unique<MemoryPskStore>();
    media_ = std::make_unique<CallMediaEngine>();
    media_->SetSkipDeviceOpenForTest(true);
    dial_ = std::make_unique<FakeDialRegistry>();
    transport_ = std::make_unique<FakeCallMediaTransport>();

    CallDeliveryPorts delivery;
    delivery.send_user_message = [this](const std::string& thread_id, const std::string& text,
                                        const SendRelayOptions& options) -> Roe<ThreadMessage> {
      if (fail_sends_) {
        return Error("offline (test)");
      }
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      msg.payload_json = options.payload_json.value_or("");
      msg.timestamp = util::NowUnixMs();
      ++sent_control_messages_;
      last_sent_payload_ = msg.payload_json;
      return msg;
    };
    delivery.sync_inbox_from_wake = [this](bool /*force*/) { ++inbox_syncs_; };

    csm_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *sessions_, *keys_,
                                                std::move(delivery), *psk_, *media_);
    csm_->SetOutbox(csm_events_.Get());  // the fixture plays the stack
    bridge_ = std::make_unique<CallMediaBridge>(csm_->AsMediaHost(), *sessions_, *keys_, *media_, *transport_,
                                                dial_.get(), nullptr);
    bridge_->SetDirectArmingPorts(csm_->DirectArmingPorts());
    bridge_->SetSeatPorts(csm_->DirectSeatPorts());
    csm_->SetDirectMediaPorts(TestDirectMediaPorts(bridge_.get(), &csm_->SeatForTest()));
    csm_->SetDirectDriver(bridge_.get());

    dial_->force_dialable["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWPeer";
  }

  void TearDown() override {
    if (bridge_) {
      // Brief wait so Connect/StartSfu workers release profile.db before remove_all (Windows).
      bridge_->PrepareForTeardown(500);
    }
    if (csm_) {
      csm_->SetDirectMediaPorts({});
      csm_->SetDirectDriver(nullptr);
    }
    // Always Stop — StartSfu may arm capture after PrepareForTeardown cleared media_call_id_.
    if (media_) {
      media_->Stop();
    }
    // Drain UI/worker replies while CSM/bridge still alive (avoid UAF on late Accept/Decline).
    (void)AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000));
    // Drain only proves a marker task passed; a worker task posted by the body (offerer Accept
    // roster fan-out → SendCallDirectMessage → SqliteThreadStore) can still be running on
    // another pool thread. Join the pool before destroying anything it may touch — ASan:
    // heap-use-after-free in SqliteThreadStore::UpsertThread from FanOutToJoinedAndRinging
    // (Windows CI SEGFAULT in InboundAcceptAsOffererSchedulesDirectMedia).
    AppRuntime::Shutdown();
    bridge_.reset();
    csm_.reset();
    transport_.reset();
    dial_.reset();
    media_.reset();
    psk_.reset();
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

  Roe<ThreadMessage> MakeInviteMessage(const std::string& call_id,
                                       std::optional<int64_t> expires_at = std::nullopt,
                                       std::optional<bool> video_allowed = std::nullopt) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.video_allowed = video_allowed.value_or(false);
    invite.origin_thread_id = "thread:origin";
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
    return CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallInvite, "Incoming call",
                                                *detail, "account:peer");
  }

  /** Answerer Invite → AcceptClicked → Leave → Ended. Returns false on assertion failure path. */
  void RunAnswererInviteAcceptLeave(const std::string& call_id) {
    auto msg = MakeInviteMessage(call_id);
    ASSERT_TRUE(msg) << msg.error().message;
    ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
    ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

    csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
    csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
    DrainUntil([&]() {
      return csm_->Live().Phase() == CallPhase::JoinedLocal || csm_->Live().Phase() == CallPhase::MediaPending ||
             csm_->Live().Phase() == CallPhase::MediaConnecting || csm_->Live().Phase() == CallPhase::InCall ||
             media_->IsActive();
    });
    ASSERT_NE(csm_->Live().Phase(), CallPhase::Accepting) << csm_->LastError();
    ASSERT_NE(csm_->Live().Phase(), CallPhase::Ringing) << csm_->LastError();
    EXPECT_TRUE(bridge_->MediaAttempted(call_id));

    csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle);
    DrainUntil([&]() {
      auto session = sessions_->LoadSession(call_id);
      return session && session->has_value() && (*session)->state == CallSessionState::Ended;
    });
    auto session = sessions_->LoadSession(call_id);
    ASSERT_TRUE(session && session->has_value());
    EXPECT_EQ((*session)->state, CallSessionState::Ended);
    DrainUntil([&]() {
      auto after = csm_->ActiveLocalCall();
      return after && !after->has_value();
    });
  }

  /** The call the device shows (active, else the ring); empty when idle. */
  std::string ShownCallId() const {
    const LiveCall* shown = csm_->Live().Shown();
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
    csm_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"});  // as StartCall does
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<CallSessionStore> sessions_;
  std::unique_ptr<CallMediaKeyStore> keys_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<MemoryPskStore> psk_;
  std::unique_ptr<CallMediaEngine> media_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<CallMediaBridge> bridge_;
  std::unique_ptr<CallSessionManager> csm_;
  CallsLoopbackOutbox<SessionEvent> csm_events_{[this](SessionEvent& event) {
    if (csm_) {
      csm_->Handle(event);
    }
  }};
  std::string local_identity_;
  int sent_control_messages_ = 0;
  /** Every call-control send fails (e.g. the relay is unreachable). */
  bool fail_sends_ = false;
  std::atomic<int> inbox_syncs_{0};  // bumped from the deferred-key poll on workers
  std::string last_sent_payload_;
};

TEST_F(CallSessionInboundComposeTest, InboundInviteCreatesPendingAndRingingSession) {
  const std::string call_id = "call:invite-1";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg) << msg.error().message;

  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer")) << "inbound invite";

  auto pending = csm_->TopPendingInvite();
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

TEST_F(CallSessionInboundComposeTest, StaleInviteDroppedByRelayAge) {
  const std::string call_id = "call:stale";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);

  const int64_t created = 1'000;
  const int64_t relay_now = created + kDefaultCallInviteTtlMs + kCallInviteRelayAgeSlackMs + 1;
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer", created, relay_now));

  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, DeclineClearsPendingInvite) {
  const std::string call_id = "call:decline";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  ASSERT_TRUE(csm_->DeclineInvite(call_id)) << "decline";
  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
  EXPECT_GE(sent_control_messages_, 1);
}

// V037 intents: an Accept click while its accept is in flight is not a second accept; the ring stays
// suppressed until the accept answers.
TEST_F(CallSessionInboundComposeTest, AcceptClickedDedupesWhileInFlight) {
  const std::string call_id = "call:dedupe";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  int parks = 0;
  std::function<void(bool)> park_done;
  csm_->SetParkCircuit([&](int /*timeout_ms*/, std::function<void(bool)> done) {
    ++parks;
    park_done = std::move(done);
  });

  csm_->Apply(CallLifecycleEvent::AcceptClicked, {});  // the ring's call
  EXPECT_EQ(csm_->AcceptingCallId(), call_id);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Accepting);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  EXPECT_EQ(parks, 1) << "the second click started no second accept";

  ASSERT_TRUE(park_done);
  park_done(true);
  AppRuntime::RunAllOwnerTasks();
  EXPECT_TRUE(csm_->AcceptingCallId().empty());
  EXPECT_NE(csm_->Live().Phase(), CallPhase::Ringing);
  EXPECT_TRUE(csm_->LastError().empty());
}

// PR #239 review: two inbound bundles can each raise PeerReconnected before the first queued resume
// runs. Only a failed call resumes; a resume that fails after the call went live again must not
// knock it back to ConnectFailed.
TEST_F(CallSessionInboundComposeTest, ADuplicateResumeThatFailsLeavesTheResumedCallInCall) {
  const std::string call_id = "call:resume-dup";
  SeedOffererRingingCall(call_id);
  LiveCalls& live = csm_->LiveCallsForTest();
  live.MarkJoined(call_id);
  live.NoteMediaConnected(call_id);
  live.NoteMediaFailed(call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::ConnectFailed);

  csm_->Apply(CallLifecycleEvent::PeerReconnected, call_id);
  csm_->Apply(CallLifecycleEvent::PeerReconnected, call_id);  // ignored: no longer failed
  live.NoteMediaConnected(call_id);  // the first resume committed the peer's stream
  AppRuntime::RunAllOwnerTasks();    // the queued resume finds nothing to resume and fails
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::InCall);
}

// thread-ownership t2a: Accept waits for the circuit park without blocking the calls owner — other
// owner work (here: a ring notice and inbound control) runs while the park is outstanding, and
// nothing is committed (no CallAccept, not Joined) until the park answers.
TEST_F(CallSessionInboundComposeTest, AcceptAwaitsCircuitParkWithoutBlockingTheOwner) {
  const std::string call_id = "call:park";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  std::function<void(bool)> park_done;
  csm_->SetParkCircuit([&](int /*timeout_ms*/, std::function<void(bool)> done) { park_done = std::move(done); });
  std::optional<Roe<void>> accepted;
  csm_->AcceptInviteAsync(call_id, [&](Roe<void> result) { accepted = std::move(result); });
  ASSERT_TRUE(park_done) << "accept asked for the circuit park";
  EXPECT_FALSE(accepted.has_value()) << "accept must not finish before the park";
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  EXPECT_FALSE(self && self->has_value() && (*self)->state == CallParticipantState::Joined);

  bool owner_free = false;
  AppRuntime::PostToOwnerOrRun(OwnerThreadId::MediaSessions, [&]() { owner_free = true; });
  AppRuntime::RunAllOwnerTasks();
  EXPECT_TRUE(owner_free) << "the calls owner kept serving while accept awaited the park";

  park_done(true);
  AppRuntime::RunAllOwnerTasks();
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(*accepted) << accepted->error().message;
  self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
  EXPECT_GE(sent_control_messages_, 1) << "CallAccept sent after the park";
}

TEST_F(CallSessionInboundComposeTest, InviteAcceptLeaveProductCompose) {
  // B-CALL-DIRECT product glue: Invite → AcceptClicked → media → Leave → Idle.
  const std::string call_id = "call:compose";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Ringing);

  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  // The accept runs on the calls owner at once (no park here): the ring is gone, the call connecting.
  EXPECT_NE(csm_->Live().Phase(), CallPhase::Ringing);
  EXPECT_NE(csm_->Live().Phase(), CallPhase::Idle);

  DrainUntil([&]() {
    return csm_->Live().Phase() == CallPhase::JoinedLocal || csm_->Live().Phase() == CallPhase::MediaPending ||
           csm_->Live().Phase() == CallPhase::MediaConnecting || csm_->Live().Phase() == CallPhase::InCall ||
           media_->IsActive();
  });

  EXPECT_NE(csm_->Live().Phase(), CallPhase::Accepting) << csm_->LastError();
  EXPECT_NE(csm_->Live().Phase(), CallPhase::Ringing) << csm_->LastError();
  EXPECT_TRUE(bridge_->MediaAttempted(call_id));

  auto active = csm_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, call_id);

  // Duplicate invite while Joined must not demote to Ringing.
  auto again = MakeInviteMessage(call_id);
  ASSERT_TRUE(again);
  ASSERT_TRUE(csm_->ApplyInboundControl(*again, "account:peer"));
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);

  csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle);
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Ended);

  DrainUntil([&]() {
    auto after = csm_->ActiveLocalCall();
    return after && !after->has_value();
  });
  auto after = csm_->ActiveLocalCall();
  ASSERT_TRUE(after);
  EXPECT_FALSE(after->has_value());
}

TEST_F(CallSessionInboundComposeTest, InviteAcceptLeaveKCycleTeardown) {
  // B-TEARDOWN: Leave→Idle then a second Invite→Accept→Leave must succeed (no orphan bind).
  RunAnswererInviteAcceptLeave("call:cycle-1");
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::Idle);
  EXPECT_FALSE(csm_->Seat().IsBound("call:cycle-1"));

  RunAnswererInviteAcceptLeave("call:cycle-2");
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::Idle);
  auto after = csm_->ActiveLocalCall();
  ASSERT_TRUE(after);
  EXPECT_FALSE(after->has_value());
}

TEST_F(CallSessionInboundComposeTest, WireSkewStaleInviteDropped) {
  const std::string call_id = "call:wire-stale";
  const int64_t expires = util::NowUnixMs() - kCallInviteWireSkewSlackMs - 1;
  auto msg = MakeInviteMessage(call_id, expires);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto pending = csm_->TopPendingInvite();
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

  csm_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"});  // as StartCall does
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  csm_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::InCall);

  CallEndedDetail ended;
  ended.call_id = call_id;
  ended.duration_ms = 1500;
  auto detail = CallControlCodec::EncodeEnded(ended);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallEnded, "Call ended", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle)
      << "CallEnded → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(ShownCallId().empty());
}

TEST_F(CallSessionInboundComposeTest, InboundAcceptAsOffererSchedulesDirectMedia) {
  // B-CALL-DIRECT offerer glue only: inbound CallAccept → schedule_start(..., offerer=true)
  // + session/peer Joined. Do not drive Bridge BeginSession / StartSfu / Connect grace —
  // those belong to Bridge/engine tests (and TearDown must not inherit an armed Connect).
  const std::string call_id = "call:offerer-accept";
  SeedOffererRingingCall(call_id);

  std::string scheduled_call;
  std::string scheduled_peer;
  bool scheduled_offerer = false;
  int schedule_calls = 0;
  // The call's coordinator starts the 1:1 path through the direct driver: spy on that.
  struct SpyDriver final : CallDirectDriver {
    std::function<void(const std::string&, const std::string&, bool)> on_start;
    void ScheduleDirectStart(const std::string& cid, const std::string& peer, bool offerer) override {
      on_start(cid, peer, offerer);
    }
    void ReleaseDirectTransport(const CallMediaSeat::Token&) override {}
    void ReleaseDirectTransport() override {}
    void StopMeshMedia(const std::string&) override {}
    Roe<void> RetryMeshMedia(const std::string&) override { return {}; }
    Roe<void> ResumeMeshMediaFromInbound(const std::string&) override { return {}; }
  } spy;
  spy.on_start = [&](const std::string& cid, const std::string& peer, bool offerer) {
    ++schedule_calls;
    scheduled_call = cid;
    scheduled_peer = peer;
    scheduled_offerer = offerer;
  };
  csm_->SetDirectDriver(&spy);

  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  csm_->LiveCallsForTest().SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, "test");

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
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  EXPECT_EQ(schedule_calls, 1);
  EXPECT_EQ(scheduled_call, call_id);
  EXPECT_EQ(scheduled_peer, "account:peer");
  EXPECT_TRUE(scheduled_offerer);
  csm_->SetDirectDriver(bridge_.get());

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Active);
  auto peer = sessions_->FindParticipant(call_id, "account:peer");
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ((*peer)->state, CallParticipantState::Joined);
}

// video-voice-choice: callee answers an incoming video invite as voice-only. AcceptInvite must
// narrow its own session and send Accept.video_allowed=false.
TEST_F(CallSessionInboundComposeTest, VoiceOnlyAnswerNarrowsCalleeSessionAndSendsFalse) {
  const std::string call_id = "call:voice-only-narrow";
  auto msg = MakeInviteMessage(call_id, std::nullopt, /*video_allowed=*/true);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  csm_->SetPendingAcceptVoiceOnly(true);
  std::optional<Roe<void>> accepted;
  csm_->AcceptInviteAsync(call_id, [&](Roe<void> result) { accepted = std::move(result); });
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(*accepted) << accepted->error().message;

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_FALSE((*session)->video_allowed);

  auto sent = DecodeSentAccept(last_sent_payload_);
  ASSERT_TRUE(sent);
  ASSERT_TRUE(sent->video_allowed.has_value());
  EXPECT_FALSE(*sent->video_allowed);
}

// A voice-only accept on a call that was already voice-only writes nothing extra on the wire.
TEST_F(CallSessionInboundComposeTest, VoiceOnlyAnswerOnVoiceCallWritesNothing) {
  const std::string call_id = "call:voice-only-noop";
  auto msg = MakeInviteMessage(call_id);  // voice invite (video_allowed=false)
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  csm_->SetPendingAcceptVoiceOnly(true);
  std::optional<Roe<void>> accepted;
  csm_->AcceptInviteAsync(call_id, [&](Roe<void> result) { accepted = std::move(result); });
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(*accepted) << accepted->error().message;

  auto sent = DecodeSentAccept(last_sent_payload_);
  ASSERT_TRUE(sent);
  EXPECT_FALSE(sent->video_allowed.has_value());

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_FALSE((*session)->video_allowed);
}

// A normal (video) accept on a video invite leaves the field unset and keeps video_allowed=true.
TEST_F(CallSessionInboundComposeTest, VideoAnswerLeavesFieldUnset) {
  const std::string call_id = "call:video-answer-normal";
  auto msg = MakeInviteMessage(call_id, std::nullopt, /*video_allowed=*/true);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  std::optional<Roe<void>> accepted;
  csm_->AcceptInviteAsync(call_id, [&](Roe<void> result) { accepted = std::move(result); });
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(*accepted) << accepted->error().message;

  auto sent = DecodeSentAccept(last_sent_payload_);
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
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto video_allowed = csm_->VideoAllowedForCall(call_id);
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
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

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
  ASSERT_TRUE(csm_->ApplyInboundControl(*narrow_msg, "account:peer"));
  auto after_narrow = csm_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(after_narrow && after_narrow->has_value());
  ASSERT_FALSE(**after_narrow);

  CallAcceptDetail replay_accept;  // relay replay of the same Accept, missing the field this time
  replay_accept.call_id = call_id;
  replay_accept.identity = "account:peer";
  auto replay_detail = CallControlCodec::EncodeAccept(replay_accept);
  ASSERT_TRUE(replay_detail);
  auto replay_msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted",
                                                          *replay_detail, "account:peer");
  ASSERT_TRUE(replay_msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*replay_msg, "account:peer"));
  auto after_replay = csm_->VideoAllowedForCall(call_id);
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
  ASSERT_TRUE(csm_->ApplyInboundControl(*plain_msg, "account:peer"));
  auto fresh_video_allowed = csm_->VideoAllowedForCall(call_id2);
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
  session.origin_group_id = "group:out";  // started from a group thread
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
  csm_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer1", "account:peer2"});  // as StartCall does
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
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer1"));

  auto video_allowed = csm_->VideoAllowedForCall(call_id);
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
    ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  }
  auto group_allowed = csm_->VideoAllowedForCall(group_call);
  ASSERT_TRUE(group_allowed && group_allowed->has_value());
  EXPECT_TRUE(**group_allowed) << "a group-thread call is never narrowed call-wide";
  auto direct_allowed = csm_->VideoAllowedForCall(direct_call);
  ASSERT_TRUE(direct_allowed && direct_allowed->has_value());
  EXPECT_FALSE(**direct_allowed) << "a direct call narrows even with a guest already invited";
}

TEST_F(CallSessionInboundComposeTest, InboundMediaKeyUnwrapsAndKicksAnswerer) {
  ByteVector master(32);
  for (size_t i = 0; i < master.size(); ++i) {
    master[i] = static_cast<uint8_t>(0x40 + i);
  }
  psk_->SeedMasterPsk(master);

  const std::string call_id = "call:media-key";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() {
    return csm_->Live().Phase() == CallPhase::JoinedLocal || csm_->Live().Phase() == CallPhase::MediaPending ||
           csm_->Live().Phase() == CallPhase::MediaConnecting || media_->IsActive();
  });
  // Accept without pre-seeded key → MediaPending / KeyWait.
  EXPECT_TRUE(bridge_->MediaAttempted(call_id));
  if (media_->IsActive()) {
    // Rare: key race; still exercise unwrap path below.
  } else {
    EXPECT_TRUE(csm_->Live().Phase() == CallPhase::MediaPending ||
                csm_->Live().Phase() == CallPhase::JoinedLocal ||
                csm_->Live().Phase() == CallPhase::MediaConnecting);
  }

  auto session_key = SessionKeyDeriver::Derive(master, CryptoChannel::E2ePublic, 1);
  ASSERT_TRUE(session_key) << session_key.error().message;
  const ByteVector media_key = TestMediaKey();
  auto wrapped = CallMediaKeyStore::WrapKeyB64(*session_key, media_key, call_id, 1, "mk:1");
  ASSERT_TRUE(wrapped);

  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = 1;
  key_detail.media_key_id = "mk:1";
  key_detail.wrapped_key_b64 = *wrapped;
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  ASSERT_TRUE(key_json);
  auto key_msg =
      CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallMediaKey, "Media key", *key_json,
                                           "account:peer");
  ASSERT_TRUE(key_msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*key_msg, "account:peer"));

  DrainUntil([&]() { return media_->IsActive(); });
  EXPECT_TRUE(media_->IsActive());
  auto stored = keys_->LoadEpochKey(call_id, 1);
  ASSERT_TRUE(stored && stored->has_value());
  EXPECT_EQ(**stored, media_key);

  csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
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

  csm_->LiveCallsForTest().AdmitPlaced(call_id, {"account:peer"});  // as StartCall does
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  csm_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::InCall);
  ASSERT_EQ(ShownCallId(), call_id);

  CallLeaveDetail leave;
  leave.call_id = call_id;
  leave.identity = "account:peer";
  auto detail = CallControlCodec::EncodeLeave(leave);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:1", CallControlType::CallLeave, "Left", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle)
      << "peer Leave → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(ShownCallId().empty());
  EXPECT_FALSE((csm_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, AcceptSecondInviteEndsPriorActiveCall) {
  // B-CONFLICT product glue: Accept B while Joined on A ends A via LeaveCallIfActiveExcept.
  const std::string call_a = "call:conflict-a";
  const std::string call_b = "call:conflict-b";

  auto msg_a = MakeInviteMessage(call_a);
  ASSERT_TRUE(msg_a);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg_a, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_a, 1, TestMediaKey()));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_a);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_a);
  DrainUntil([&]() {
    auto active = csm_->ActiveLocalCall();
    // Wait for AcceptInvite UI completion — reduces SQLITE_BUSY vs concurrent invite B upsert.
    return active && active->has_value() && (*active)->call_id == call_a &&
           csm_->Live().Phase() != CallPhase::Accepting;
  });
  ASSERT_TRUE(csm_->ActiveLocalCall()->has_value());

  auto msg_b = MakeInviteMessage(call_b);
  ASSERT_TRUE(msg_b);
  {
    auto applied_b = csm_->ApplyInboundControl(*msg_b, "account:peer");
    ASSERT_TRUE(applied_b) << (applied_b ? "" : applied_b.error().message);
  }
  ASSERT_TRUE(keys_->PutEpochKey(call_b, 1, TestMediaKey()));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_b);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_b);
  DrainUntil([&]() {
    auto active = csm_->ActiveLocalCall();
    return active && active->has_value() && (*active)->call_id == call_b;
  });

  auto a = sessions_->LoadSession(call_a);
  ASSERT_TRUE(a && a->has_value());
  EXPECT_EQ((*a)->state, CallSessionState::Ended);
  auto b = sessions_->LoadSession(call_b);
  ASSERT_TRUE(b && b->has_value());
  EXPECT_NE((*b)->state, CallSessionState::Ended);
  auto active = csm_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, call_b);
  // Ending A must not RemoteEnded-clobber Accepting/Joined B.
  EXPECT_NE(csm_->Live().Phase(), CallPhase::Idle);
  EXPECT_EQ(ShownCallId(), call_b);

  csm_->Apply(CallLifecycleEvent::LeaveClicked, call_b);
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

  csm_->AbandonOrphanedCallsAfterRestart();

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  auto top = csm_->TopPendingInvite();
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

  csm_->LiveCallsForTest().AdmitInvited(pending.call_id, {"account:peer"});  // as the inbound invite does
  csm_->Apply(CallLifecycleEvent::InviteSeen, pending.call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::Ringing);
  ASSERT_TRUE((csm_->Live().Phase() != CallPhase::Idle));

  csm_->SweepExpiredInvites();

  auto top = csm_->TopPendingInvite();
  ASSERT_TRUE(top);
  EXPECT_FALSE(top->has_value());
  auto self = sessions_->FindParticipant(pending.call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Missed);
  auto loaded = sessions_->LoadSession(pending.call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle)
      << "CALLS expire → Idle (no DeclineClicked); got " << CallPhaseName(csm_->Live().Phase());
  EXPECT_FALSE((csm_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachIgnoredWhileDirectConnecting) {
  const std::string call_id = "call:sfu-ignored";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return bridge_->MediaAttempted(call_id) || media_->IsActive(); });
  ASSERT_TRUE(csm_->Live().AllowsDirectPath());

  CallSfuAttachDetail attach;
  attach.call_id = call_id;
  attach.hop_peer_id = "12D3KooWHop";
  attach.hop_multiaddr = "/ip4/10.0.0.9/tcp/4001/p2p/12D3KooWHop";
  attach.quote_id = "q:1";
  attach.publisher_stream_id = 1;
  auto detail = CallControlCodec::EncodeSfuAttach(attach);
  ASSERT_TRUE(detail);
  auto attach_msg =
      CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallSfuAttach, "SFU", *detail,
                                           "account:peer");
  ASSERT_TRUE(attach_msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*attach_msg, "account:peer"));

  // V037/V038: DirectConnecting Status must not flip to hop attach.
  EXPECT_TRUE(csm_->Live().AllowsDirectPath());
  EXPECT_FALSE(csm_->Live().AllowsHopPath());
  EXPECT_FALSE(csm_->IsSfuAttached());
}

TEST_F(CallSessionInboundComposeTest, InboundHopRefuseEndsBoundCall) {
  const std::string call_id = "call:hop-refuse";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return csm_->Seat().IsBound(call_id) || media_->IsActive(); });
  ASSERT_TRUE(csm_->Seat().IsBound(call_id) || media_->IsActive());

  CallHopRefuseDetail refuse;
  refuse.call_id = call_id;
  refuse.identity = local_identity_;
  refuse.reason = "no_shared_hop";
  refuse.message = "No shared hop";
  auto detail = CallControlCodec::EncodeHopRefuse(refuse);
  ASSERT_TRUE(detail);
  auto refuse_msg =
      CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallHopRefuse, "Refuse", *detail,
                                           "account:peer");
  ASSERT_TRUE(refuse_msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*refuse_msg, "account:peer"));

  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  auto err = csm_->TakeLastMediaError();
  ASSERT_TRUE(err.has_value());
  EXPECT_EQ(*err, "No shared hop");
}

TEST_F(CallSessionInboundComposeTest, LegacySdpAndIceIgnored) {
  auto sdp = CallControlCodec::BuildSystemMessage(
      "thread:1", CallControlType::CallSdp, "sdp", R"({"call_id":"call:x","sdp":"v=0","type":"offer"})",
      "account:peer");
  ASSERT_TRUE(sdp);
  ASSERT_TRUE(csm_->ApplyInboundControl(*sdp, "account:peer"));

  auto ice = CallControlCodec::BuildSystemMessage(
      "thread:1", CallControlType::CallIce, "ice",
      R"({"call_id":"call:x","candidate":"a","sdp_mid":"0","sdp_mline_index":0})", "account:peer");
  ASSERT_TRUE(ice);
  ASSERT_TRUE(csm_->ApplyInboundControl(*ice, "account:peer"));

  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, DeclineClickedClearsPendingViaLifecycle) {
  const std::string call_id = "call:decline-life";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Ringing);

  csm_->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  DrainUntil([&]() {
    auto pending = csm_->TopPendingInvite();
    return pending && !pending->has_value() && csm_->Live().Phase() == CallPhase::Idle;
  });
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle);
  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, InboundDeclineClearsOffererOutboundCalling) {
  // CALLS: peer Decline → offerer Idle (no local LeaveClicked / TTL wait).
  const std::string call_id = "call:inbound-decline";
  SeedOffererRingingCall(call_id);
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling);
  ASSERT_TRUE(csm_->ActiveLocalCall()->has_value());

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = "account:peer";
  auto detail = CallControlCodec::EncodeDecline(decline);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined",
                                                  *detail, "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle)
      << "inbound CallDecline must EndCallLocal → RemoteEnded";
  EXPECT_TRUE(ShownCallId().empty());
  auto active = csm_->ActiveLocalCall();
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

  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling);

  CallDeclineDetail decline;
  decline.call_id = call_id;
  decline.identity = "account:peer";
  auto detail = CallControlCodec::EncodeDecline(decline);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined",
                                                  *detail, "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_NE((*loaded)->state, CallSessionState::Ended)
      << "first Decline must keep session while account:other still Ringing";
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling)
      << "got phase=" << CallPhaseName(csm_->Live().Phase());
  EXPECT_EQ(ShownCallId(), call_id);

  CallDeclineDetail decline_other;
  decline_other.call_id = call_id;
  decline_other.identity = "account:other";
  auto detail_other = CallControlCodec::EncodeDecline(decline_other);
  ASSERT_TRUE(detail_other);
  auto msg_other = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallDecline, "Declined",
                                                        *detail_other, "account:other");
  ASSERT_TRUE(msg_other);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg_other, "account:other"));

  loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle);
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
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));

  auto invite = MakeInviteMessage(call_id);
  ASSERT_TRUE(invite);
  ASSERT_TRUE(csm_->ApplyInboundControl(*invite, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return media_->IsActive() || bridge_->MediaAttempted(call_id); });

  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachFailedDispatched) {
  const std::string call_id = "call:sfu-fail";
  SeedOffererRingingCall(call_id);
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);

  CallSfuAttachFailedDetail fail;
  fail.call_id = call_id;
  fail.identity = "account:peer";
  fail.failed_hop_peer_id = "12D3KooWHop";
  fail.error = "attach_failed";
  auto detail = CallControlCodec::EncodeSfuAttachFailed(fail);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallSfuAttachFailed, "fail",
                                                  *detail, "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
}

TEST_F(CallSessionInboundComposeTest, RetryP2pMediaAfterConnectFailed) {
  const std::string call_id = "call:retry";
  SeedOffererRingingCall(call_id);
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  csm_->LiveCallsForTest().SetMediaStatus(call_id, CallMediaStatus::DirectConnecting, "test");

  CallAcceptDetail accept;
  accept.call_id = call_id;
  accept.identity = "account:peer";
  auto detail = CallControlCodec::EncodeAccept(accept);
  ASSERT_TRUE(detail);
  auto msg = CallControlCodec::BuildSystemMessage("thread:out", CallControlType::CallAccept, "Accepted", *detail,
                                                  "account:peer");
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  DrainUntil([&]() { return bridge_->MediaAttempted(call_id) || media_->IsActive(); });
  ASSERT_TRUE(bridge_->MediaAttempted(call_id));

  csm_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::ConnectFailed);
  EXPECT_FALSE(csm_->Live().AllowsDirectPath());

  csm_->Apply(CallLifecycleEvent::RetryClicked, call_id);
  EXPECT_TRUE(csm_->Live().AllowsDirectPath()) << "RetryClicked re-arms DirectConnecting";
  DrainUntil([&]() {
    return csm_->Live().Phase() == CallPhase::MediaConnecting || csm_->Live().Phase() == CallPhase::InCall;
  });
  EXPECT_TRUE(csm_->Live().Phase() == CallPhase::MediaConnecting ||
              csm_->Live().Phase() == CallPhase::InCall)
      << "phase=" << CallPhaseName(csm_->Live().Phase()) << " err=" << csm_->LastError();

  csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
}

// thread-ownership t2a: mesh start / stop swap the session manager's ports while other threads use
// them. Each use takes one snapshot, so a check and the call it guards never straddle a swap — the
// old rebind-in-place could call an empty port (bad_function_call) or destroy a running one (TSan).
TEST_F(CallSessionInboundComposeTest, PortSwapsDuringUseNeverCallAnEmptyPort) {
  CallDirectMediaPorts ports;
  std::atomic<int> polls{0};
  ports.media_path_kind = []() { return std::string("direct"); };
  ports.is_connect_failed = []() { return false; };
  ports.connect_missing_mic = []() { return false; };
  ports.poll_connect_health = [&polls]() { polls.fetch_add(1); };
  ports.media_attempted = [](const std::string&) { return false; };
  std::atomic<bool> stop{false};
  std::thread swapper([&]() {
    for (int i = 0; !stop.load(); ++i) {
      csm_->SetDirectMediaPorts(i % 2 == 0 ? ports : CallDirectMediaPorts{});
    }
  });
  for (int i = 0; i < 20000; ++i) {
    const std::string kind = csm_->MediaPathKind();
    EXPECT_TRUE(kind.empty() || kind == "direct");
    (void)csm_->IsP2pConnectFailed();
    (void)csm_->P2pConnectMissingMic();
    (void)csm_->MediaAttemptedThisProcess("call:swap");
    csm_->PollP2pConnectHealth();
  }
  stop.store(true);
  swapper.join();
  csm_->SetDirectMediaPorts({});
}

TEST_F(CallSessionInboundComposeTest, StartCallOutboundCreatesSessionAndInvite) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  // Windows: thread id is a directory name under threads/ — no ':' (illegal path char).
  thread.id = "thread-dm-out";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = csm_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  EXPECT_FALSE(started->call_id.empty());
  EXPECT_EQ(started->state, CallSessionState::Ringing);
  EXPECT_EQ(started->origin_thread_id, thread.id);

  auto self = sessions_->FindParticipant(started->call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
  EXPECT_GE(sent_control_messages_, 1);
  auto key = keys_->LoadEpochKey(started->call_id, 1);
  ASSERT_TRUE(key && key->has_value());
}

// PR #240 review: a new call whose invites cannot go out must leave the current call alone, and must
// not stay behind ringing.
TEST_F(CallSessionInboundComposeTest, StartCallThatCannotInviteKeepsTheCurrentCall) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  for (const char* id : {"thread-dm-a", "thread-dm-b"}) {
    Thread thread;
    thread.id = id;
    thread.kind = ThreadKind::Direct;
    thread.title = "Peer";
    thread.updated_at = util::NowUnixMs();
    ASSERT_TRUE(store_->UpsertThread(thread));
  }
  auto a = csm_->StartCall("thread-dm-a", false, {"account:peer"});
  ASSERT_TRUE(a) << a.error().message;

  fail_sends_ = true;
  auto b = csm_->StartCall("thread-dm-b", false, {"account:other"});
  ASSERT_FALSE(b) << "the invite could not be sent";

  const LiveCall* active = csm_->Live().Active();
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

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesAutoLeavesOutboundUnanswered) {
  // CALLS: OutboundCalling + no media past TTL → Leave via Sweep (not GUI LeaveClicked).
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread-dm-ttl";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = csm_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  csm_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling);

  // Age the session past invite TTL without waiting 60s.
  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  (*session)->created_at = util::NowUnixMs() - kDefaultCallInviteTtlMs - 1;
  ASSERT_TRUE(sessions_->UpsertSession(**session));

  csm_->SweepExpiredInvites();
  DrainUntil([&]() {
    return csm_->Live().Phase() == CallPhase::Idle && !csm_->ActiveLocalCall()->has_value();
  });
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::Idle);
  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_FALSE((csm_->Live().Phase() != CallPhase::Idle));
}

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesSkipsOutboundBeforeTtl) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread-dm-ttl-early";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = csm_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  csm_->Apply(CallLifecycleEvent::OutboundStarted, started->call_id);
  ASSERT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling);

  csm_->SweepExpiredInvites();
  EXPECT_EQ(csm_->Live().Phase(), CallPhase::OutboundCalling);
  EXPECT_TRUE(csm_->ActiveLocalCall()->has_value());
}

TEST_F(CallSessionInboundComposeTest, MuteAndVideoControlsOnActiveMedia) {
  const std::string call_id = "call:mute-video";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  // Allow video for enable gate (invite default voice → flip session flag).
  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  (*session)->video_allowed = true;
  ASSERT_TRUE(sessions_->UpsertSession(**session));

  csm_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  csm_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return media_->IsActive(); });
  ASSERT_TRUE(media_->IsActive());
  EXPECT_TRUE(csm_->MediaAttemptedThisProcess(call_id));

  auto peer = csm_->PeerIdentityForCall(call_id);
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ(**peer, "account:peer");
  auto video_ok = csm_->VideoAllowedForCall(call_id);
  ASSERT_TRUE(video_ok && video_ok->has_value());
  EXPECT_TRUE(**video_ok);

  const int sent_before = sent_control_messages_;
  ASSERT_TRUE(csm_->SetLocalAudioMuted(true));
  EXPECT_TRUE(media_->IsMuted());
  auto self = sessions_->FindParticipant(call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_TRUE((*self)->media.audio_muted);
  EXPECT_GT(sent_control_messages_, sent_before) << "mute should fan-out CallRoster";

  ASSERT_TRUE(csm_->SetLocalAudioMuted(false));
  EXPECT_FALSE(media_->IsMuted());

  // Camera may fail headless — gate must still accept video_allowed before device open.
  auto enable = csm_->SetLocalVideoEnabled(true, 0);
  if (enable) {
    EXPECT_TRUE(media_->IsCameraEnabled());
    ASSERT_TRUE(csm_->SetLocalVideoEnabled(false, 0));
  } else {
    EXPECT_FALSE(enable.error().message.empty());
  }

  // Voice-only session rejects enable.
  auto voice_only = sessions_->LoadSession(call_id);
  ASSERT_TRUE(voice_only && voice_only->has_value());
  (*voice_only)->video_allowed = false;
  ASSERT_TRUE(sessions_->UpsertSession(**voice_only));
  auto denied = csm_->SetLocalVideoEnabled(true, 0);
  EXPECT_FALSE(denied);
  EXPECT_NE(denied.error().message.find("Video is not allowed"), std::string::npos);

  ASSERT_TRUE(csm_->RequestVideoRefresh(call_id, local_identity_));
  ASSERT_TRUE(csm_->RequestVideoRefresh(call_id, {}));

  csm_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    auto row = sessions_->LoadSession(call_id);
    return row && row->has_value() && (*row)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, MuteWithoutActiveMediaFails) {
  auto muted = csm_->SetLocalAudioMuted(true);
  EXPECT_FALSE(muted);
  EXPECT_NE(muted.error().message.find("No active call media"), std::string::npos);
}

} // namespace
} // namespace pbr
