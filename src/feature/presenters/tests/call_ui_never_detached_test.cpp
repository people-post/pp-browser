// DEV/PLAN-call-ui-never-detached.md 4: the call screen on the real call stack while reading this device's call from
// the store fails. While the engine has the call open, the screen (and the "in a call" notification it drives) never
// goes; End still ends it without the store; a screen that lost its call takes it back on the next frame; and in the
// ordinary flows (a ring, calling out, a blocked invite, End & accept, couldn't connect, the peer hanging up) the
// last-resort guard never fires and the notification follows the screen as before.
//
// Media runs with its devices skipped (SetSkipDeviceOpenForTest): no microphone or camera is opened.
#include "feature/presenters/CallPresenter.h"
#include "feature/presenters/CallUiAttachGuard.h"

#include "domain/call_media/ICallMediaTransport.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallTypes.h"
#include "lib/calls/testing/FailingCallSessionStore.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/MeshConfig.h"
#include "common/media/MediaSessionsOwner.h"
#include "common/runtime/AppRuntime.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/Utilities.h"

#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

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

class FakeDialRegistry final : public mesh::IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    endpoints[peer_key] = multiaddr;
    return {};
  }
  bool IsDialable(const std::string& peer_key) const override {
    return endpoints.count(peer_key) > 0 || connected.count(peer_key) > 0;
  }
  bool IsConnected(const std::string& peer_key) const override { return connected.count(peer_key) > 0; }
  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    const auto it = endpoints.find(peer_key);
    return it != endpoints.end() ? std::optional<std::string>(it->second) : std::nullopt;
  }
  void ClearDialBackoff(const std::string& /*peer_key*/) override {}
  void AbortInflightDial(const std::string& /*peer_key*/) override {}
  void ClearPeerCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> connected;
};

/** The 1:1 media link: connects at once. */
class FakeCallMediaTransport final : public ICallMediaTransport {
public:
  void Start() override {}
  void Stop() override {}
  void SetInboundHandler(CallMediaInboundHandler handler) override { inbound = std::move(handler); }
  void ClearInboundHandler() override { inbound = {}; }
  bool IsActive() const override { return active; }
  CallMediaDirectConnectParams ActiveParams() const override { return active_params; }
  CallMediaSessionPhase Phase() const override {
    return active ? CallMediaSessionPhase::MediaReady : CallMediaSessionPhase::Idle;
  }
  void Detach(const std::string& /*call_id*/) override { active = false; }
  void DetachAll() override { Detach({}); }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(CallMediaRoe)> on_done, int /*timeout_ms*/) override {
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
                      uint64_t /*instance*/) override {
    return {};
  }

  bool active = false;
  CallMediaDirectConnectParams active_params;
  CallMediaInboundHandler inbound;
};

class CallUiNeverDetachedTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(MediaSessionsManualRuntimeConfig());
    AppRuntime::InitializeUI();

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_ui_attach_" + util::GenerateUuid());
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "call-ui-attach-test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    auto loaded = identity_->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    local_identity_ = loaded->account_id;

    stack_ = std::make_unique<CallStack>();
    CallStores stores = InMemoryCallStores();
    auto sessions = std::make_unique<FailingCallSessionStore>();
    sessions_ = sessions.get();
    stores.sessions = std::move(sessions);
    ASSERT_TRUE(stack_->InitializeStores(std::move(stores)));
    ui_ = std::make_unique<CallUiBackend>(*stack_);

    CallStackDeps deps;
    deps.threads = store_.get();
    deps.transcript = store_.get();
    deps.contacts = contacts_.get();
    deps.account = identity_.get();
    deps.delivery.send_control = [](const std::string& /*peer*/, const ThreadMessage& /*control*/,
                                    bool /*urgent*/) -> Roe<void> { return {}; };
    deps.delivery.send_control_to_many = ControlSendsOneByOne(deps.delivery.send_control);
    deps.delivery.send_to_thread = [](const std::string& thread_id, const std::string& text,
                                      const SendRelayOptions& options) -> Roe<ThreadMessage> {
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.sender_contact_id = kLocalSelfContactId;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      return msg;
    };
    deps.delivery.resolve_peer_thread = [](const std::string& peer) -> Roe<std::string> { return "dm:" + peer; };
    deps.delivery.sync_inbox_from_wake = [](bool /*force*/) {};
    deps.mesh_config = []() { return std::make_shared<const MeshConfig>(); };
    deps.mesh = []() -> mesh::MeshHost* { return nullptr; };
    deps.list_directory_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
    deps.list_dht_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
    deps.seed_dial_ok = []() { return false; };
    deps.prefetch_peer_reachability = [](const std::string&) {};
    deps.sync_mobile_ephemeral_listen = []() {};
    deps.system_controls = &controls_;
    deps.connectivity = &connectivity_;
    deps.media_relay = &media_relay_;
    mesh::AddressDisclosurePolicy everyone;
    everyone.audience = mesh::DirectAudience::Everyone;
    disclosure_.Publish(everyone);
    deps.address_disclosure = &disclosure_;
    stack_->BuildSessions(deps);
    ASSERT_TRUE(ui_->Available());
    ASSERT_NE(stack_->Engine(), nullptr);
    stack_->Engine()->SetSkipDeviceOpenForTest(true); // never a real microphone or camera

    transport_ = std::make_unique<FakeCallMediaTransport>();
    dial_ = std::make_unique<FakeDialRegistry>();
    dial_->connected["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWPeer";
    stack_->BindTestMediaPath(transport_.get(), dial_.get());

    presenter_ = std::make_unique<CallPresenter>();
    CallFunctionalPorts ports;
    ports.initialized = []() { return true; };
    ports.backend = [this]() { return ui_.get(); };
    ports.local_relay_identity = [this]() { return std::optional<std::string>(local_identity_); };
    CallPresenter::Callbacks callbacks;
    callbacks.notify = [this](const CallNotice& notice) { notices_.push_back(notice); };
    presenter_->BindCallbacks(std::move(callbacks));
    presenter_->BindPorts(std::move(ports));
  }

  void TearDown() override {
    (void)AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000));
    if (stack_) {
      stack_->AbortCallMediaForShutdown();
      stack_->Shutdown();
    }
    ui_.reset();
    stack_.reset();
    transport_.reset();
    dial_.reset();
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
    presenter_.reset();
    identity_.reset();
    contacts_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  void DrainUntil(const std::function<bool()>& done, int max_ms = 4000) {
    for (int i = 0; i < std::max(1, max_ms / 10); ++i) {
      AppRuntime::RunUIAndOwnerTasks();
      if (done()) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    AppRuntime::RunUIAndOwnerTasks();
  }

  bool EngineOpen() const { return EngineHasOpenCall(ui_->Available(), ui_->ActiveCallId(), ui_->Phase()); }

  /**
   * `frames` frames as CallController runs them, a second apart on the guard's clock: the owner's queued work, the
   * presenter's tick, then the guard. Fails on any guard action, and (when `service_as_before`) on an ongoing-call
   * service that differs from the screen's own call (the rule before this change).
   */
  /** The service decision CallController makes now (OngoingCallWanted, the call being left not counted). */
  bool ServiceOn() const {
    const EngineCall engine = EngineCallOf(*ui_->State());
    const bool engine_call = engine.open && engine.call_id != presenter_->LeavingCallId();
    return OngoingCallWanted(!presenter_->ActiveCallId().empty(), engine_call);
  }

  /** One frame of CallController's: the presenter's tick, then the guard (a second later on its clock). */
  CallUiAttachGuard::Step Frame() {
    presenter_->Tick();
    guard_ms_ += 1000;
    const EngineCall engine = EngineCallOf(*ui_->State());
    return guard_.Feed({.engine_open = engine.open,
                        .engine_call_id = engine.open ? engine.call_id : std::string(),
                        .ui_call_id = presenter_->ActiveCallId(),
                        .leaving_call_id = presenter_->LeavingCallId(),
                        .steady_ms = guard_ms_});
  }

  /**
   * `frames` frames as CallController runs them, the owner's queued work drained before each. Fails on any guard
   * action, and (when `service_as_before`) on an ongoing-call service that differs from the screen's own call (the rule
   * before this change).
   */
  void Frames(int frames, bool service_as_before = true) {
    for (int i = 0; i < frames; ++i) {
      AppRuntime::RunUIAndOwnerTasks();
      const auto step = Frame();
      EXPECT_FALSE(step.report_detached || step.hang_up || step.stop_again)
          << "guard fired phase=" << CallPhaseName(ui_->Phase()) << " screen_call=" << presenter_->ActiveCallId();
      if (service_as_before) {
        EXPECT_EQ(ServiceOn(), !presenter_->ActiveCallId().empty()) << "phase=" << CallPhaseName(ui_->Phase());
      }
    }
  }

  Roe<void> Ingest(const CallControlType type, const std::string& detail, const std::string& sender) {
    auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", type, "call control", detail, sender);
    if (!msg) {
      return msg.error();
    }
    const auto family = controls_.Find(*msg);
    if (!family || !family->apply) {
      return Error("inbound not bound");
    }
    auto applied = family->apply(*msg, SystemControlContext{sender, std::nullopt, std::nullopt});
    AppRuntime::RunUIAndOwnerTasks();
    return applied;
  }

  Roe<void> IngestInvite(const std::string& call_id) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    auto detail = CallControlCodec::EncodeInvite(invite);
    if (!detail) {
      return detail.error();
    }
    if (auto key = stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()); !key) {
      return key.error();
    }
    return Ingest(CallControlType::CallInvite, *detail, "account:peer");
  }

  /** A ring from the peer, answered on the screen: the call is open in the engine and on screen. */
  void InCall(const std::string& call_id) {
    ASSERT_TRUE(IngestInvite(call_id));
    presenter_->Refresh();
    DrainUntil([&]() { return presenter_->Screen().kind == CallScreen::Kind::Ringing; });
    ASSERT_EQ(presenter_->Screen().kind, CallScreen::Kind::Ringing);
    ASSERT_EQ(presenter_->Accept(/*voice_only=*/true, call_id), call_id);
    DrainUntil([&]() {
      presenter_->Tick();
      return EngineOpen() && presenter_->ActiveCallId() == call_id;
    });
    ASSERT_TRUE(EngineOpen()) << CallPhaseName(ui_->Phase()) << " " << ui_->LastError();
    ASSERT_EQ(ui_->ActiveCallId(), call_id);
    ASSERT_EQ(presenter_->ActiveCallId(), call_id);
    ASSERT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
  }

  bool HasNotice(const CallNotice::Kind kind) const {
    for (const CallNotice& n : notices_) {
      if (n.kind == kind) {
        return true;
      }
    }
    return false;
  }

  SystemControlRegistry controls_;
  std::filesystem::path data_dir_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  mesh::MeshConnectivity connectivity_;
  mesh::MeshMediaRelay media_relay_{connectivity_};
  mesh::AddressDisclosureGate disclosure_;
  std::unique_ptr<CallStack> stack_;
  FailingCallSessionStore* sessions_ = nullptr;
  std::unique_ptr<CallUiBackend> ui_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<CallPresenter> presenter_;
  std::string local_identity_;
  std::vector<CallNotice> notices_;
  CallUiAttachGuard guard_;
  int64_t guard_ms_ = 0;
};

TEST_F(CallUiNeverDetachedTest, OneReadShowingNoCallChangesNothingOnScreen) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:one-empty"));
  const CallScreen before = presenter_->Screen();
  sessions_->hide_active = true; // the read succeeds and lists no call
  presenter_->Refresh();
  sessions_->hide_active = false;
  EXPECT_EQ(presenter_->ActiveCallId(), "call:one-empty");
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(presenter_->Screen().call_id, before.call_id);
  EXPECT_EQ(presenter_->Screen().peer_identity, before.peer_identity) << "the screen as it was";
  Frames(3);
}

TEST_F(CallUiNeverDetachedTest, OneFailedReadChangesNothingOnScreen) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:one-error"));
  sessions_->fail_participants = true;
  presenter_->Refresh();
  sessions_->fail_participants = false;
  EXPECT_EQ(presenter_->ActiveCallId(), "call:one-error");
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
  EXPECT_EQ(presenter_->Screen().call_id, "call:one-error");
  Frames(3);
}

TEST_F(CallUiNeverDetachedTest, ReadsFailingForGoodKeepTheScreenAndEndStillEndsWithoutTheStore) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:all-fail"));
  ASSERT_TRUE(stack_->Engine()->IsActive());
  sessions_->fail_active = true;
  sessions_->fail_participants = true;
  Frames(10); // ten seconds of frames
  EXPECT_EQ(presenter_->ActiveCallId(), "call:all-fail");
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
  EXPECT_TRUE(EngineOpen());

  EXPECT_EQ(presenter_->Leave(), "call:all-fail");
  DrainUntil([&]() { return ui_->Phase() == CallPhase::Idle && !stack_->Engine()->IsActive(); });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->Engine()->IsActive()) << "the media stopped";
  Frames(3);
  EXPECT_TRUE(presenter_->ActiveCallId().empty());
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::None);
}

TEST_F(CallUiNeverDetachedTest, AScreenThatLostItsCallTakesItBackOnTheNextFrame) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:lost"));
  bool detach = true;
  presenter_->SetDevDetachUi([&detach]() { return std::exchange(detach, false); });
  presenter_->Refresh();
  ASSERT_TRUE(presenter_->ActiveCallId().empty()) << "dropped as something else might";
  presenter_->Tick();
  EXPECT_EQ(presenter_->ActiveCallId(), "call:lost") << "the tick refreshes while the engine has the call";
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
}

TEST_F(CallUiNeverDetachedTest, ALostCallComesBackWhileTheStoreFailsToo) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:lost-unread"));
  bool detach = true;
  presenter_->SetDevDetachUi([&detach]() { return std::exchange(detach, false); });
  presenter_->Refresh();
  ASSERT_TRUE(presenter_->ActiveCallId().empty());
  sessions_->fail_participants = true;
  presenter_->Tick();
  EXPECT_EQ(presenter_->ActiveCallId(), "call:lost-unread");
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall) << "a plain in-call screen with End";
  EXPECT_EQ(presenter_->Screen().call_id, "call:lost-unread");
}

TEST_F(CallUiNeverDetachedTest, ThePeerHangingUpStillClosesTheScreenAndSaysSo) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:peer-ends"));
  CallLeaveDetail leave;
  leave.call_id = "call:peer-ends";
  leave.identity = "account:peer";
  auto detail = CallControlCodec::EncodeLeave(leave);
  ASSERT_TRUE(detail);
  ASSERT_TRUE(Ingest(CallControlType::CallLeave, *detail, "account:peer"));
  DrainUntil([&]() {
    presenter_->Tick();
    presenter_->Refresh();
    return presenter_->Screen().kind == CallScreen::Kind::None;
  });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::None);
  EXPECT_TRUE(presenter_->ActiveCallId().empty());
  EXPECT_TRUE(HasNotice(CallNotice::Kind::PeerEnded)) << "the peer's end is told as before";
  Frames(8);
}

TEST_F(CallUiNeverDetachedTest, ARingNotAnsweredNeverTriggersTheGuard) {
  ASSERT_TRUE(IngestInvite("call:ringing"));
  presenter_->Refresh();
  DrainUntil([&]() { return presenter_->Screen().kind == CallScreen::Kind::Ringing; });
  ASSERT_EQ(presenter_->Screen().kind, CallScreen::Kind::Ringing);
  Frames(8);
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_FALSE(EngineOpen()) << CallPhaseName(ui_->Phase());
}

TEST_F(CallUiNeverDetachedTest, CallingOutUnansweredNeverTriggersTheGuard) {
  Thread thread;
  thread.id = "thread-out";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));
  std::optional<Roe<CallSession>> started;
  ui_->StartCall(thread.id, false, {"account:peer"}, [&started](Roe<CallSession> r) { started = std::move(r); });
  DrainUntil([&]() { return started.has_value(); });
  ASSERT_TRUE(started && *started) << (started ? started->error().message : "no report");
  presenter_->Refresh();
  ASSERT_EQ(ui_->Phase(), CallPhase::OutboundCalling);
  EXPECT_EQ(presenter_->ActiveCallId(), (*started)->call_id);
  Frames(8);
  EXPECT_EQ(presenter_->ActiveCallId(), (*started)->call_id);
  (void)presenter_->Leave();
  DrainUntil([&]() { return ui_->Phase() == CallPhase::Idle; });
  Frames(3);
}

TEST_F(CallUiNeverDetachedTest, ABlockedInviteNeverTriggersTheGuard) {
  stack_->SetInboundCallAudience(mesh::InboundAudience::Nobody);
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_TRUE(IngestInvite("call:blocked"));
  presenter_->Refresh();
  Frames(8);
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::None);
}

TEST_F(CallUiNeverDetachedTest, EndAndAcceptNeverTriggersTheGuard) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:first"));
  ASSERT_TRUE(IngestInvite("call:second"));
  presenter_->Refresh();
  DrainUntil([&]() {
    presenter_->Tick();
    return presenter_->Screen().kind == CallScreen::Kind::Ringing;
  });
  ASSERT_EQ(presenter_->Screen().kind, CallScreen::Kind::Ringing);
  EXPECT_TRUE(presenter_->Screen().conflict) << "answering ends the call we are in";
  Frames(3);
  ASSERT_EQ(presenter_->Accept(/*voice_only=*/true, "call:second"), "call:second");
  for (int i = 0; i < 40 && presenter_->ActiveCallId() != "call:second"; ++i) {
    Frames(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(presenter_->ActiveCallId(), "call:second");
  EXPECT_EQ(ui_->ActiveCallId(), "call:second");
  Frames(8);
}

TEST_F(CallUiNeverDetachedTest, CouldNotConnectNeverTriggersTheGuard) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:failed"));
  stack_->RunOnOwner([](CallDevice& calls) { calls.LiveCallsForTest().NoteMediaFailed("call:failed"); });
  DrainUntil([&]() {
    presenter_->Tick();
    return ui_->Phase() == CallPhase::ConnectFailed;
  });
  ASSERT_EQ(ui_->Phase(), CallPhase::ConnectFailed);
  EXPECT_FALSE(EngineOpen()) << "a failed call is not an open one";
  Frames(8);
  EXPECT_EQ(presenter_->ActiveCallId(), "call:failed") << "the couldn't-connect screen as before";
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
}

// Review 1: End posts the stop and the leave to the calls owner; until it ran them the engine and the row still read
// "in the call". Meanwhile the screen never takes the call back, the guard does nothing, the service is off.
TEST_F(CallUiNeverDetachedTest, AHungUpCallIsNeverTakenBackBeforeTheOwnerRanTheLeave) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:hung-up"));
  ASSERT_TRUE(ServiceOn());
  notices_.clear(); // the ring's own notice
  EXPECT_EQ(presenter_->Leave(), "call:hung-up");
  ASSERT_TRUE(EngineOpen()) << "the owner has not run the leave yet";
  EXPECT_EQ(presenter_->LeavingCallId(), "call:hung-up");
  for (int i = 0; i < 8; ++i) { // eight seconds of frames, the owner never drained
    const auto step = Frame();
    EXPECT_FALSE(step.report_detached || step.hang_up) << "frame " << i;
    EXPECT_TRUE(presenter_->ActiveCallId().empty()) << "taken back at frame " << i;
    EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::None) << "frame " << i;
    EXPECT_FALSE(ServiceOn()) << "the service came back at frame " << i;
  }
  EXPECT_TRUE(notices_.empty()) << "a notice after End (\"couldn't connect\", \"call ended\"…)";

  DrainUntil([&]() { return ui_->Phase() == CallPhase::Idle && !stack_->Engine()->IsActive(); });
  ASSERT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->Engine()->IsActive());
  Frames(2);
  EXPECT_TRUE(presenter_->LeavingCallId().empty()) << "let go by the engine: forgotten";

  ASSERT_NO_FATAL_FAILURE(InCall("call:next")); // the next call works as ever
  Frames(3);
}

// The engine keeps a hung-up call open (its stop did not take): only its media is stopped again, every 2 s, 3 times.
TEST_F(CallUiNeverDetachedTest, AHungUpCallTheEngineKeepsOpenIsOnlyStoppedAgain) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:stuck"));
  (void)presenter_->Leave();
  int stops = 0;
  for (int i = 0; i < 12; ++i) { // the owner never runs the leave
    const auto step = Frame();
    EXPECT_FALSE(step.report_detached || step.hang_up) << "frame " << i;
    if (step.stop_again) {
      EXPECT_EQ(step.call_id, "call:stuck");
      ++stops;
    }
  }
  EXPECT_EQ(stops, CallUiAttachGuard::kMaxStopRetries);
  EXPECT_TRUE(presenter_->ActiveCallId().empty());
}

TEST_F(CallUiNeverDetachedTest, HangingUpWhileTheStoreKeepsFailingStillEndsTheCall) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:end-unread"));
  sessions_->fail_active = true;
  sessions_->fail_participants = true;
  (void)presenter_->Leave();
  for (int i = 0; i < 3; ++i) {
    EXPECT_FALSE(Frame().hang_up);
    EXPECT_TRUE(presenter_->ActiveCallId().empty());
  }
  DrainUntil([&]() { return ui_->Phase() == CallPhase::Idle && !stack_->Engine()->IsActive(); });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->Engine()->IsActive()) << "the media stopped";
  Frames(3);
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::None);
}

// Review 2: the guard ends the engine's call by its id — not whatever call the screen holds.
TEST_F(CallUiNeverDetachedTest, TheGuardEndsTheEnginesCallByItsId) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:engine"));
  presenter_->SetDevDetachUi([]() { return true; }); // the screen drops its id after every refresh
  std::optional<CallUiAttachGuard::Step> hang_up;
  for (int i = 0; i < 10 && !hang_up; ++i) {
    AppRuntime::RunUIAndOwnerTasks();
    const auto step = Frame();
    if (step.hang_up) {
      hang_up = step;
    }
  }
  ASSERT_TRUE(hang_up) << "the guard never hung up";
  EXPECT_EQ(hang_up->call_id, "call:engine");
  presenter_->HangUpEngineCall(hang_up->call_id); // what CallController does with it
  presenter_->SetDevDetachUi({});
  DrainUntil([&]() { return ui_->Phase() == CallPhase::Idle && !stack_->Engine()->IsActive(); });
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->Engine()->IsActive());
  Frames(3);
}

TEST_F(CallUiNeverDetachedTest, HangingUpAnotherIdLeavesTheScreensCallAlone) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:kept"));
  presenter_->HangUpEngineCall("call:not-this-one");
  DrainUntil([&]() { return false; }, 200);
  EXPECT_TRUE(EngineOpen()) << "the call on screen was ended for another id";
  EXPECT_EQ(ui_->ActiveCallId(), "call:kept");
  presenter_->Tick();
  EXPECT_EQ(presenter_->ActiveCallId(), "call:kept");
}

// No flap: on while in the call, off from the frame End is pressed, never back on.
TEST_F(CallUiNeverDetachedTest, TheServiceGoesOffOnceAtEndAndStaysOff) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:service"));
  std::vector<bool> on;
  for (int i = 0; i < 3; ++i) {
    AppRuntime::RunUIAndOwnerTasks();
    (void)Frame();
    on.push_back(ServiceOn());
  }
  (void)presenter_->Leave();
  for (int i = 0; i < 3; ++i) { // before the owner ran the leave
    (void)Frame();
    on.push_back(ServiceOn());
  }
  for (int i = 0; i < 4; ++i) { // and after
    AppRuntime::RunUIAndOwnerTasks();
    (void)Frame();
    on.push_back(ServiceOn());
  }
  EXPECT_EQ(on, (std::vector<bool>{true, true, true, false, false, false, false, false, false, false}));
}

// Review 4: the call left and joined again under the same id before any tick (the latch is dropped where a call is
// answered, started or joined): it shows, the service is on, the guard does nothing.
TEST_F(CallUiNeverDetachedTest, ACallLeftAndJoinedAgainBeforeAnyTickShows) {
  ASSERT_NO_FATAL_FAILURE(InCall("call:again"));
  (void)presenter_->Leave();
  ASSERT_EQ(presenter_->LeavingCallId(), "call:again");
  // Joined again at once (the answer of its new ring), the owner not drained: the engine still has it open.
  (void)presenter_->Accept(/*voice_only=*/true, "call:again");
  EXPECT_TRUE(presenter_->LeavingCallId().empty()) << "answering a call drops the latch";
  for (int i = 0; i < 6; ++i) {
    const auto step = Frame();
    EXPECT_FALSE(step.report_detached || step.hang_up || step.stop_again) << "frame " << i;
  }
  EXPECT_EQ(presenter_->ActiveCallId(), "call:again");
  EXPECT_EQ(presenter_->Screen().kind, CallScreen::Kind::InCall);
  EXPECT_TRUE(ServiceOn());
}

// The latch never lasts: 15 s on the steady clock, then a call the engine still holds is shown again (its mic open).
TEST_F(CallUiNeverDetachedTest, TheLeavingLatchRunsOutAfterFifteenSeconds) {
  int64_t now_ms = 1'000'000;
  presenter_->SetSteadyClockForTest([&now_ms]() { return now_ms; });
  ASSERT_NO_FATAL_FAILURE(InCall("call:stuck-open"));
  (void)presenter_->Leave();
  now_ms += CallPresenter::kLeavingLatchMs - 1;
  presenter_->Tick(); // the owner never runs the leave
  EXPECT_EQ(presenter_->LeavingCallId(), "call:stuck-open");
  EXPECT_TRUE(presenter_->ActiveCallId().empty());
  now_ms += 1;
  presenter_->Tick();
  EXPECT_TRUE(presenter_->LeavingCallId().empty()) << "dropped after 15 s";
  presenter_->Tick();
  EXPECT_EQ(presenter_->ActiveCallId(), "call:stuck-open") << "still open in the engine: on screen again";
}

} // namespace
} // namespace pbr
