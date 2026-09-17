#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallMediaSeat.h"
#include "feature/calls/CallSessionManager.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/AnnounceLiveJoin.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "common/thread/ThreadRecordTypes.h"
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
  void ClearCallMediaCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> force_dialable;
};

class FakeCallMediaTransport final : public ICallMediaTransport {
public:
  void Start() override { started = true; }
  void Stop() override { started = false; }
  void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) override {
    inbound = std::move(handler);
  }
  void ClearInboundHandler() override { inbound = {}; }
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
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> inbound;
};

void DrainUntil(const std::function<bool()>& done, int max_ms = 4000) {
  const int slices = std::max(1, max_ms / 10);
  for (int i = 0; i < slices; ++i) {
    AppRuntime::RunUITasks();
    if (done()) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  AppRuntime::RunUITasks();
}

class CallSessionInboundComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize();
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
    dial_ = std::make_unique<FakeDialRegistry>();
    transport_ = std::make_unique<FakeCallMediaTransport>();
    seat_ = std::make_unique<CallMediaSeat>();
    lifecycle_ = std::make_unique<CallLifecycle>();

    CallDeliveryPorts delivery;
    delivery.send_user_message = [this](const std::string& thread_id, const std::string& text,
                                        const SendRelayOptions& options) -> Roe<ThreadMessage> {
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
    bridge_ = std::make_unique<CallMediaBridge>(csm_->AsMediaHost(), *sessions_, *keys_, *media_, *transport_,
                                                dial_.get(), nullptr);
    bridge_->SetLifecycle(lifecycle_.get());
    bridge_->SetMediaSeat(seat_.get());
    csm_->SetCallMediaBridge(bridge_.get());
    csm_->SetMediaSeat(seat_.get());
    csm_->SetLifecycle(lifecycle_.get());
    lifecycle_->Bind(csm_.get());

    seat_->SetTeardownHooks(
        [this](const std::string& call_id) {
          if (csm_) {
            csm_->TopologyOnMediaStoppedForSeat(call_id);
          }
        },
        [this](const std::string& call_id, uint64_t epoch_at_post, bool force) {
          if (!force && seat_ && seat_->Epoch() != epoch_at_post) {
            return;
          }
          if (bridge_) {
            bridge_->StopMeshMedia(call_id);
          }
        });

    dial_->force_dialable["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWPeer";
  }

  void TearDown() override {
    if (lifecycle_) {
      lifecycle_->ClearBinding();
    }
    if (bridge_) {
      bridge_->PrepareForTeardown(0);
    }
    if (csm_) {
      csm_->SetCallMediaBridge(nullptr);
      csm_->SetLifecycle(nullptr);
      csm_->SetMediaSeat(nullptr);
    }
    bridge_.reset();
    csm_.reset();
    lifecycle_.reset();
    seat_.reset();
    transport_.reset();
    dial_.reset();
    if (media_ && (media_->IsActive() || media_->IsSfuMode())) {
      media_->Stop();
    }
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
    std::filesystem::remove_all(data_dir_);
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
  }

  Roe<ThreadMessage> MakeInviteMessage(const std::string& call_id,
                                       std::optional<int64_t> expires_at = std::nullopt) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.video_allowed = false;
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

    lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
    lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
    DrainUntil([&]() {
      return lifecycle_->Phase() == CallPhase::JoinedLocal || lifecycle_->Phase() == CallPhase::MediaPending ||
             lifecycle_->Phase() == CallPhase::MediaConnecting || lifecycle_->Phase() == CallPhase::InCall ||
             media_->IsActive();
    });
    ASSERT_NE(lifecycle_->Phase(), CallPhase::Accepting) << lifecycle_->LastError();
    ASSERT_NE(lifecycle_->Phase(), CallPhase::Ringing) << lifecycle_->LastError();
    EXPECT_TRUE(bridge_->MediaAttempted(call_id));

    lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle);
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

  void SeedOffererRingingCall(const std::string& call_id) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread:out";
    session.media_mode = CallMediaMode::Voice;
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
  std::unique_ptr<CallMediaSeat> seat_;
  std::unique_ptr<CallLifecycle> lifecycle_;
  std::unique_ptr<CallMediaBridge> bridge_;
  std::unique_ptr<CallSessionManager> csm_;
  std::string local_identity_;
  int sent_control_messages_ = 0;
  int inbox_syncs_ = 0;
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

TEST_F(CallSessionInboundComposeTest, InviteAcceptLeaveProductCompose) {
  // B-CALL-DIRECT product glue: Invite → AcceptClicked → media → Leave → Idle.
  const std::string call_id = "call:compose";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Ringing);

  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Accepting);

  DrainUntil([&]() {
    return lifecycle_->Phase() == CallPhase::JoinedLocal || lifecycle_->Phase() == CallPhase::MediaPending ||
           lifecycle_->Phase() == CallPhase::MediaConnecting || lifecycle_->Phase() == CallPhase::InCall ||
           media_->IsActive();
  });

  EXPECT_NE(lifecycle_->Phase(), CallPhase::Accepting) << lifecycle_->LastError();
  EXPECT_NE(lifecycle_->Phase(), CallPhase::Ringing) << lifecycle_->LastError();
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

  lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle);
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
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(seat_->IsBound("call:cycle-1"));

  RunAnswererInviteAcceptLeave("call:cycle-2");
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::Idle);
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

  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::InCall);

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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle)
      << "CallEnded → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(lifecycle_->ActiveCallId().empty());
}

TEST_F(CallSessionInboundComposeTest, InboundAcceptAsOffererSchedulesDirectMedia) {
  const std::string call_id = "call:offerer-accept";
  SeedOffererRingingCall(call_id);

  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);

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

  DrainUntil([&]() { return bridge_->MediaAttempted(call_id) || media_->IsActive(); });
  EXPECT_TRUE(bridge_->MediaAttempted(call_id));

  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_EQ((*session)->state, CallSessionState::Active);
  auto peer = sessions_->FindParticipant(call_id, "account:peer");
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ((*peer)->state, CallParticipantState::Joined);
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

  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() {
    return lifecycle_->Phase() == CallPhase::JoinedLocal || lifecycle_->Phase() == CallPhase::MediaPending ||
           lifecycle_->Phase() == CallPhase::MediaConnecting || media_->IsActive();
  });
  // Accept without pre-seeded key → MediaPending / KeyWait.
  EXPECT_TRUE(bridge_->MediaAttempted(call_id));
  if (media_->IsActive()) {
    // Rare: key race; still exercise unwrap path below.
  } else {
    EXPECT_TRUE(lifecycle_->Phase() == CallPhase::MediaPending ||
                lifecycle_->Phase() == CallPhase::JoinedLocal ||
                lifecycle_->Phase() == CallPhase::MediaConnecting);
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

  lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
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

  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::InCall);
  ASSERT_EQ(lifecycle_->ActiveCallId(), call_id);

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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle)
      << "peer Leave → EndCallLocal must Apply RemoteEnded (no local LeaveClicked)";
  EXPECT_TRUE(lifecycle_->ActiveCallId().empty());
  EXPECT_FALSE(lifecycle_->WantEphemeralListen());
}

TEST_F(CallSessionInboundComposeTest, AcceptSecondInviteEndsPriorActiveCall) {
  // B-CONFLICT product glue: Accept B while Joined on A ends A via LeaveCallIfActiveExcept.
  const std::string call_a = "call:conflict-a";
  const std::string call_b = "call:conflict-b";

  auto msg_a = MakeInviteMessage(call_a);
  ASSERT_TRUE(msg_a);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg_a, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_a, 1, TestMediaKey()));
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_a);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_a);
  DrainUntil([&]() {
    auto active = csm_->ActiveLocalCall();
    return active && active->has_value() && (*active)->call_id == call_a;
  });
  ASSERT_TRUE(csm_->ActiveLocalCall()->has_value());

  auto msg_b = MakeInviteMessage(call_b);
  ASSERT_TRUE(msg_b);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg_b, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_b, 1, TestMediaKey()));
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_b);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_b);
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
  EXPECT_NE(lifecycle_->Phase(), CallPhase::Idle);
  EXPECT_EQ(lifecycle_->ActiveCallId(), call_b);

  lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_b);
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

  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, pending.call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::Ringing);
  ASSERT_TRUE(lifecycle_->WantEphemeralListen());

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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle)
      << "CALLS expire → Idle (no DeclineClicked); got " << CallPhaseName(lifecycle_->Phase());
  EXPECT_FALSE(lifecycle_->WantEphemeralListen());
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachIgnoredWhileDirectConnecting) {
  const std::string call_id = "call:sfu-ignored";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return bridge_->MediaAttempted(call_id) || media_->IsActive(); });
  ASSERT_TRUE(lifecycle_->AllowsDirectPath());

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
  EXPECT_TRUE(lifecycle_->AllowsDirectPath());
  EXPECT_FALSE(lifecycle_->AllowsHopPath());
  EXPECT_FALSE(csm_->IsSfuAttached());
}

TEST_F(CallSessionInboundComposeTest, InboundHopRefuseEndsBoundCall) {
  const std::string call_id = "call:hop-refuse";
  auto msg = MakeInviteMessage(call_id);
  ASSERT_TRUE(msg);
  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return seat_->IsBound(call_id) || media_->IsActive(); });
  ASSERT_TRUE(seat_->IsBound(call_id) || media_->IsActive());

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
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Ringing);

  lifecycle_->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  DrainUntil([&]() {
    auto pending = csm_->TopPendingInvite();
    return pending && !pending->has_value() && lifecycle_->Phase() == CallPhase::Idle;
  });
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle);
  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallSessionInboundComposeTest, InboundDeclineClearsOffererOutboundCalling) {
  // CALLS: peer Decline → offerer Idle (no local LeaveClicked / TTL wait).
  const std::string call_id = "call:inbound-decline";
  SeedOffererRingingCall(call_id);
  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling);
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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle)
      << "inbound CallDecline must EndCallLocal → RemoteEnded";
  EXPECT_TRUE(lifecycle_->ActiveCallId().empty());
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

  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling);

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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling)
      << "got phase=" << CallPhaseName(lifecycle_->Phase());
  EXPECT_EQ(lifecycle_->ActiveCallId(), call_id);

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
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle);
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
  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() { return media_->IsActive() || bridge_->MediaAttempted(call_id); });

  ASSERT_TRUE(csm_->ApplyInboundControl(*msg, "account:peer"));
}

TEST_F(CallSessionInboundComposeTest, InboundSfuAttachFailedDispatched) {
  const std::string call_id = "call:sfu-fail";
  SeedOffererRingingCall(call_id);
  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);

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
  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);

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

  lifecycle_->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::ConnectFailed);
  EXPECT_FALSE(lifecycle_->AllowsDirectPath());

  lifecycle_->Apply(CallLifecycleEvent::RetryClicked, call_id);
  EXPECT_TRUE(lifecycle_->AllowsDirectPath()) << "RetryClicked re-arms DirectConnecting";
  DrainUntil([&]() {
    return lifecycle_->Phase() == CallPhase::MediaConnecting || lifecycle_->Phase() == CallPhase::InCall;
  });
  EXPECT_TRUE(lifecycle_->Phase() == CallPhase::MediaConnecting ||
              lifecycle_->Phase() == CallPhase::InCall)
      << "phase=" << CallPhaseName(lifecycle_->Phase()) << " err=" << lifecycle_->LastError();

  lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    auto session = sessions_->LoadSession(call_id);
    return session && session->has_value() && (*session)->state == CallSessionState::Ended;
  });
}

TEST_F(CallSessionInboundComposeTest, BroadcastArmAndAcceptLiveAnnounceJoin) {
  AnnounceLiveJoinPlan plan;
  plan.call_id = "call:broadcast-1";
  plan.publisher_peer_id = "12D3KooWPublisher";
  plan.topic_id = "topic:live";
  plan.program_id = "prog:1";
  plan.hop_peer_id = "12D3KooWHop";
  plan.media_epoch = 1;

  auto armed = csm_->ArmJoinFromLiveAnnounce(plan);
  ASSERT_TRUE(armed) << armed.error().message;
  EXPECT_EQ(armed->call_id, plan.call_id);
  EXPECT_EQ(armed->status, "pending");

  auto pending = csm_->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, plan.call_id);

  auto session = sessions_->LoadSession(plan.call_id);
  ASSERT_TRUE(session && session->has_value());
  EXPECT_TRUE(IsBroadcastSession((*session)->session_kind));

  // Regular AcceptInvite must refuse broadcast sessions.
  auto wrong = csm_->AcceptInvite(plan.call_id);
  EXPECT_FALSE(wrong);

  ASSERT_TRUE(csm_->AcceptLiveAnnounceJoin(plan.call_id)) << "accept live announce";
  auto self = sessions_->FindParticipant(plan.call_id, local_identity_);
  ASSERT_TRUE(self && self->has_value());
  EXPECT_EQ((*self)->state, CallParticipantState::Joined);
  auto after = sessions_->LoadSession(plan.call_id);
  ASSERT_TRUE(after && after->has_value());
  EXPECT_EQ((*after)->state, CallSessionState::Active);
}

TEST_F(CallSessionInboundComposeTest, StartCallOutboundCreatesSessionAndInvite) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread:dm-out";
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

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesAutoLeavesOutboundUnanswered) {
  // CALLS: OutboundCalling + no media past TTL → Leave via Sweep (not GUI LeaveClicked).
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread:dm-ttl";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = csm_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling);

  // Age the session past invite TTL without waiting 60s.
  auto session = sessions_->LoadSession(call_id);
  ASSERT_TRUE(session && session->has_value());
  (*session)->created_at = util::NowUnixMs() - kDefaultCallInviteTtlMs - 1;
  ASSERT_TRUE(sessions_->UpsertSession(**session));

  csm_->SweepExpiredInvites();
  DrainUntil([&]() {
    return lifecycle_->Phase() == CallPhase::Idle && !csm_->ActiveLocalCall()->has_value();
  });
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::Idle);
  auto loaded = sessions_->LoadSession(call_id);
  ASSERT_TRUE(loaded && loaded->has_value());
  EXPECT_EQ((*loaded)->state, CallSessionState::Ended);
  EXPECT_FALSE(lifecycle_->WantEphemeralListen());
}

TEST_F(CallSessionInboundComposeTest, SweepExpiredInvitesSkipsOutboundBeforeTtl) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread:dm-ttl-early";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = csm_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, started->call_id);
  ASSERT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling);

  csm_->SweepExpiredInvites();
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::OutboundCalling);
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

  lifecycle_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  lifecycle_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
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
  auto enable = csm_->SetLocalVideoEnabled(true);
  if (enable) {
    EXPECT_TRUE(media_->IsCameraEnabled());
    ASSERT_TRUE(csm_->SetLocalVideoEnabled(false));
  } else {
    EXPECT_FALSE(enable.error().message.empty());
  }

  // Voice-only session rejects enable.
  auto voice_only = sessions_->LoadSession(call_id);
  ASSERT_TRUE(voice_only && voice_only->has_value());
  (*voice_only)->video_allowed = false;
  ASSERT_TRUE(sessions_->UpsertSession(**voice_only));
  auto denied = csm_->SetLocalVideoEnabled(true);
  EXPECT_FALSE(denied);
  EXPECT_NE(denied.error().message.find("Video is not allowed"), std::string::npos);

  ASSERT_TRUE(csm_->RequestVideoRefresh(call_id, local_identity_));
  ASSERT_TRUE(csm_->RequestVideoRefresh(call_id, {}));

  lifecycle_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
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
