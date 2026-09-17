#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/host/MeshControlPool.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/data/Config.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "common/thread/ThreadRecordTypes.h"

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
    dek[i] = static_cast<uint8_t>(0xd0 + i);
  }
  return dek;
}

ByteVector TestMediaKey() {
  ByteVector key(32);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(0x30 + i);
  }
  return key;
}

class FakeDialRegistry final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }
  bool IsDialable(const std::string& peer_key) const override {
    return endpoints.count(peer_key) > 0 || connected.count(peer_key) > 0;
  }
  bool IsConnected(const std::string& peer_key) const override {
    return connected.count(peer_key) > 0;
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
  std::unordered_map<std::string, bool> connected;
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

class CallUiBackendStackTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize();
    AppRuntime::InitializeUI();
    mesh_control_ = std::make_unique<MeshControlPool>(1);
    MeshControlDispatch::Install(mesh_control_.get());

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_stack_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);

    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "stack-test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    auto loaded = identity_->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    local_identity_ = loaded->account_id;

    psk_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), "stack-test");
    ASSERT_TRUE(psk_->SetDek(TestDek()));

    app_config_ = AppConfig{};
    listen_desires_ = 0;
    inbound_bound_ = false;

    stack_ = std::make_unique<CallStack>();
    ASSERT_TRUE(stack_->InitializeStores(store_->ProfileDbPath(), "stack-test"));
    ASSERT_TRUE(stack_->MediaKeys()->SetDek(TestDek()));

    ui_ = std::make_unique<CallUiBackend>(*stack_);
    EXPECT_FALSE(ui_->Available()) << "before BuildSessions";

    CallStackDeps deps;
    deps.store = store_.get();
    deps.contacts = contacts_.get();
    deps.identity = identity_.get();
    deps.psk = psk_.get();
    deps.delivery.send_user_message = [this](const std::string& thread_id, const std::string& text,
                                             const SendRelayOptions& options) -> Roe<ThreadMessage> {
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      msg.payload_json = options.payload_json.value_or("");
      msg.timestamp = util::NowUnixMs();
      ++sent_control_;
      return msg;
    };
    deps.delivery.sync_inbox_from_wake = [this](bool /*force*/) { ++inbox_syncs_; };
    deps.config = [this]() -> const AppConfig& { return app_config_; };
    deps.mesh = []() -> MeshHost* { return nullptr; };
    deps.list_directory_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
    deps.list_dht_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
    deps.seed_dial_ok = []() { return false; };
    deps.prefetch_peer_reachability = [](const std::string&) {};
    deps.sync_mobile_ephemeral_listen = [this]() { ++listen_desires_; };
    deps.bind_call_control = [this](CallControlInboundPorts ports) {
      inbound_ = std::move(ports);
      inbound_bound_ = static_cast<bool>(inbound_.apply_inbound_control);
    };

    stack_->BuildSessions(deps);
    ASSERT_TRUE(ui_->Available());
    ASSERT_TRUE(inbound_bound_);
    sessions_identity_ = ui_->SessionsIdentity();

    transport_ = std::make_unique<FakeCallMediaTransport>();
    dial_ = std::make_unique<FakeDialRegistry>();
    dial_->connected["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWPeer";
    stack_->BindTestMediaPath(transport_.get(), dial_.get());
  }

  void TearDown() override {
    ui_.reset();
    if (stack_) {
      stack_->AbortCallMediaForShutdown();
      stack_->Shutdown();
    }
    stack_.reset();
    transport_.reset();
    dial_.reset();
    if (psk_) {
      psk_->ClearDek();
    }
    psk_.reset();
    identity_.reset();
    contacts_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
    MeshControlDispatch::Uninstall();
    if (mesh_control_) {
      mesh_control_->Shutdown();
    }
    mesh_control_.reset();
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
  }

  Roe<void> IngestInvite(const std::string& call_id) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.video_allowed = false;
    invite.origin_thread_id = "thread:origin";
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    auto detail = CallControlCodec::EncodeInvite(invite);
    if (!detail) {
      return detail.error();
    }
    auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallInvite,
                                                    "Incoming call", *detail, "account:peer");
    if (!msg) {
      return msg.error();
    }
    if (!inbound_.apply_inbound_control) {
      return Error("inbound not bound");
    }
    return inbound_.apply_inbound_control(*msg, "account:peer", std::nullopt, std::nullopt);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<MeshControlPool> mesh_control_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<SqlitePskSessionStore> psk_;
  std::unique_ptr<CallStack> stack_;
  std::unique_ptr<CallUiBackend> ui_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<FakeDialRegistry> dial_;
  AppConfig app_config_;
  CallControlInboundPorts inbound_;
  bool inbound_bound_ = false;
  const void* sessions_identity_ = nullptr;
  std::string local_identity_;
  int sent_control_ = 0;
  int inbox_syncs_ = 0;
  int listen_desires_ = 0;
};

TEST_F(CallUiBackendStackTest, AvailableAndSessionsIdentityStable) {
  EXPECT_TRUE(ui_->Available());
  EXPECT_EQ(ui_->SessionsIdentity(), sessions_identity_);
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->HasActiveLocalCall());
  EXPECT_FALSE(stack_->WantEphemeralListen());
}

TEST_F(CallUiBackendStackTest, InviteAcceptLeaveThroughBackend) {
  const std::string call_id = "call:ui-compose";
  ASSERT_TRUE(IngestInvite(call_id));
  ASSERT_TRUE(stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()));

  int chrome = 0;
  int ring = 0;
  ui_->SetOnChromeRefresh([&]() { ++chrome; });
  ui_->SetOnRingChanged([&]() { ++ring; });

  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  EXPECT_EQ(ui_->Phase(), CallPhase::Ringing);
  EXPECT_EQ(ui_->LastRingCallId(), call_id);
  EXPECT_TRUE(stack_->WantEphemeralListen());
  EXPECT_GE(chrome, 1);
  EXPECT_GE(listen_desires_, 1);

  auto pending = ui_->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, call_id);

  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  EXPECT_EQ(ui_->Phase(), CallPhase::Accepting);
  EXPECT_TRUE(ui_->ShouldSuppressRing(call_id));

  DrainUntil([&]() {
    return ui_->Phase() == CallPhase::JoinedLocal || ui_->Phase() == CallPhase::MediaPending ||
           ui_->Phase() == CallPhase::MediaConnecting || ui_->Phase() == CallPhase::InCall ||
           ui_->Phase() == CallPhase::ConnectFailed;
  });
  EXPECT_NE(ui_->Phase(), CallPhase::Accepting) << ui_->LastError();
  EXPECT_NE(ui_->Phase(), CallPhase::Ringing) << ui_->LastError();

  auto active = ui_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, call_id);
  EXPECT_TRUE(stack_->HasActiveLocalCall());
  if (inbound_.has_active_local_call) {
    EXPECT_TRUE(inbound_.has_active_local_call());
  }

  auto peer = ui_->PeerIdentityForCall(call_id);
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ(**peer, "account:peer");

  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value();
  });
  EXPECT_FALSE(stack_->HasActiveLocalCall());
  EXPECT_GE(ring, 1);
}

TEST_F(CallUiBackendStackTest, InviteAcceptMediaPathThroughBackend) {
  // BindTestMediaPath: full CallStack → Bridge → StartSfu → ConnectAsync → Leave.
  const std::string call_id = "call:ui-media";
  ASSERT_TRUE(IngestInvite(call_id));
  ASSERT_TRUE(stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()));

  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);

  DrainUntil([&]() {
    return (stack_->MediaEngine() && stack_->MediaEngine()->IsActive() &&
            stack_->MediaEngine()->ActiveCallId() == call_id) ||
           ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting ||
           ui_->Phase() == CallPhase::ConnectFailed;
  });
  ASSERT_TRUE(stack_->MediaEngine());
  EXPECT_TRUE(stack_->MediaEngine()->IsActive()) << "phase=" << CallPhaseName(ui_->Phase())
                                                 << " err=" << ui_->LastError();
  EXPECT_EQ(stack_->MediaEngine()->ActiveCallId(), call_id);
  EXPECT_GE(transport_->connect_async_calls, 1);
  EXPECT_TRUE(transport_->active);

  DrainUntil([&]() {
    return ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting ||
           ui_->MediaChromeLive();
  });
  EXPECT_TRUE(ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting ||
              ui_->Phase() == CallPhase::JoinedLocal)
      << CallPhaseName(ui_->Phase());

  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value() &&
           (!stack_->MediaEngine() || !stack_->MediaEngine()->IsActive());
  });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->HasActiveLocalCall());
}

TEST_F(CallUiBackendStackTest, DeclineViaBackendClearsPending) {
  const std::string call_id = "call:ui-decline";
  ASSERT_TRUE(IngestInvite(call_id));
  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  ui_->Apply(CallLifecycleEvent::DeclineClicked, call_id);
  DrainUntil([&]() {
    auto pending = ui_->TopPendingInvite();
    return pending && !pending->has_value() && ui_->Phase() == CallPhase::Idle;
  });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  auto pending = ui_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallUiBackendStackTest, StartCallAndLeaveViaBackend) {
  ASSERT_TRUE(store_->SetDek(TestDek()));
  Thread thread;
  thread.id = "thread:ui-out";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = ui_->StartCall(thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  ui_->Apply(CallLifecycleEvent::OutboundStarted, started->call_id);
  EXPECT_EQ(ui_->Phase(), CallPhase::OutboundCalling);
  EXPECT_TRUE(stack_->WantEphemeralListen());

  auto active = ui_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, started->call_id);

  ASSERT_TRUE(ui_->LeaveCall(started->call_id));
  ui_->Apply(CallLifecycleEvent::LeaveClicked, started->call_id);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value();
  });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
}

TEST_F(CallUiBackendStackTest, BroadcastArmAcceptViaBackend) {
  AnnounceLiveJoinPlan plan;
  plan.call_id = "call:ui-bcast";
  plan.publisher_peer_id = "12D3KooWPublisher";
  plan.topic_id = "topic:1";
  plan.program_id = "prog:1";

  auto armed = ui_->ArmJoinFromLiveAnnounce(plan);
  ASSERT_TRUE(armed) << armed.error().message;
  auto pending = ui_->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, plan.call_id);

  ASSERT_TRUE(ui_->AcceptLiveAnnounceJoin(plan.call_id));
  auto active = ui_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, plan.call_id);
  EXPECT_TRUE(IsBroadcastSession(active.value()->session_kind));
}

TEST_F(CallUiBackendStackTest, UnavailableAfterResetSessions) {
  EXPECT_TRUE(ui_->Available());
  stack_->ResetSessions();
  EXPECT_FALSE(ui_->Available());
  auto pending = ui_->TopPendingInvite();
  EXPECT_FALSE(pending);
  EXPECT_EQ(pending.error().message, "Call backend unavailable");
}

} // namespace
} // namespace pbr
