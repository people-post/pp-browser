#include "feature/calls/CallStack.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/CallUiBackend.h"

#include "domain/mesh/host/MeshControlDispatch.h"
#include "domain/mesh/host/MeshControlPool.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
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
#include <deque>
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

ByteVector TestDek(uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
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
    if (peer_inbound) {
      // Simulate reverse-dial landing on the peer's CallMediaBridge inbound handler.
      CallMediaDirectConnectParams inbound_params = params;
      CallMediaDirectCallbacks inbound_cbs;
      peer_inbound(inbound_params, inbound_cbs);
      if (inbound_cbs.on_connected) {
        inbound_cbs.on_connected();
      }
    }
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
  /** When set, ConnectAsync also drives the peer stack's inbound handler (dual-stack wire). */
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> peer_inbound;
};

void DrainUntil(const std::function<bool()>& done, int max_ms = 6000) {
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

struct StackSide {
  std::filesystem::path data_dir;
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<ContactsStore> contacts;
  std::unique_ptr<IdentityStore> identity;
  std::unique_ptr<SqlitePskSessionStore> psk;
  std::unique_ptr<CallStack> stack;
  std::unique_ptr<CallUiBackend> ui;
  std::unique_ptr<FakeCallMediaTransport> transport;
  std::unique_ptr<FakeDialRegistry> dial;
  CallControlInboundPorts inbound;
  AppConfig app_config;
  std::string local_identity;
  std::deque<ThreadMessage>* outbox = nullptr;
};

class CallDualStackComposeTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize();
    AppRuntime::InitializeUI();
    mesh_control_ = std::make_unique<MeshControlPool>(1);
    MeshControlDispatch::Install(mesh_control_.get());

    BuildSide(offer_, "offer", 0xa0, &answer_inbox_);
    BuildSide(answer_, "answer", 0xb0, &offer_inbox_);

    // Each side treats the other as already connected for EnsurePeerReachable.
    offer_.dial->connected[answer_.local_identity] = true;
    offer_.dial->endpoints[answer_.local_identity] =
        "/ip4/127.0.0.1/udp/47101/adp/1.0.0/p2p/12D3KooWAnswer";
    answer_.dial->connected[offer_.local_identity] = true;
    answer_.dial->endpoints[offer_.local_identity] =
        "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWOffer";

    // Answerer reverse-dial arms offerer inbound (product: stream lands on offerer before dial).
    answer_.transport->peer_inbound =
        [this](CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
          if (!offer_.transport || !offer_.transport->inbound) {
            return;
          }
          // Leg coordinator marks the accepting transport active before invoking the handler.
          offer_.transport->active = true;
          offer_.transport->active_params = params;
          offer_.transport->inbound(params, cbs);
        };
  }

  void TearDown() override {
    TearSide(offer_);
    TearSide(answer_);
    MeshControlDispatch::Uninstall();
    if (mesh_control_) {
      mesh_control_->Shutdown();
    }
    mesh_control_.reset();
    AppRuntime::ShutdownUI();
    AppRuntime::Shutdown();
  }

  void BuildSide(StackSide& side, const char* tag, uint8_t dek_seed,
                 std::deque<ThreadMessage>* outbox) {
    side.data_dir =
        std::filesystem::temp_directory_path() / ("pp_dual_" + std::string(tag) + "_" + util::GenerateUuid());
    std::filesystem::remove_all(side.data_dir);
    std::filesystem::create_directories(side.data_dir);

    side.store = std::make_unique<SqliteThreadStore>(side.data_dir.string());
    ASSERT_TRUE(side.store->ListThreads());
    ASSERT_TRUE(side.store->SetDek(TestDek(dek_seed)));
    side.contacts = std::make_unique<ContactsStore>(side.data_dir.string());
    side.identity = std::make_unique<IdentityStore>(side.data_dir.string(), tag);
    ASSERT_TRUE(side.identity->SetDek(TestDek(dek_seed)));
    auto loaded = side.identity->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    side.local_identity = loaded->account_id;
    ASSERT_FALSE(side.local_identity.empty());

    side.psk = std::make_unique<SqlitePskSessionStore>(side.store->ProfileDbPath(), tag);
    ASSERT_TRUE(side.psk->SetDek(TestDek(dek_seed)));

    side.app_config = AppConfig{};
    side.outbox = outbox;
    side.stack = std::make_unique<CallStack>();
    ASSERT_TRUE(side.stack->InitializeStores(side.store->ProfileDbPath(), tag));
    ASSERT_TRUE(side.stack->MediaKeys()->SetDek(TestDek(dek_seed)));
    side.ui = std::make_unique<CallUiBackend>(*side.stack);

    CallStackDeps deps;
    deps.store = side.store.get();
    deps.contacts = side.contacts.get();
    deps.identity = side.identity.get();
    deps.psk = side.psk.get();
    deps.delivery.send_user_message = [&side](const std::string& thread_id, const std::string& text,
                                              const SendRelayOptions& options) -> Roe<ThreadMessage> {
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      msg.payload_json = options.payload_json.value_or("");
      msg.timestamp = util::NowUnixMs();
      if (side.outbox) {
        side.outbox->push_back(msg);
      }
      return msg;
    };
    deps.delivery.sync_inbox_from_wake = [](bool) {};
    deps.config = [&side]() -> const AppConfig& { return side.app_config; };
    deps.mesh = []() -> MeshHost* { return nullptr; };
    deps.list_directory_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
    deps.list_dht_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
    deps.seed_dial_ok = []() { return false; };
    deps.prefetch_peer_reachability = [](const std::string&) {};
    deps.sync_mobile_ephemeral_listen = []() {};
    deps.bind_call_control = [&side](CallControlInboundPorts ports) { side.inbound = std::move(ports); };

    side.stack->BuildSessions(deps);
    ASSERT_TRUE(side.ui->Available());
    ASSERT_TRUE(side.inbound.apply_inbound_control);

    side.transport = std::make_unique<FakeCallMediaTransport>();
    side.dial = std::make_unique<FakeDialRegistry>();
    side.stack->BindTestMediaPath(side.transport.get(), side.dial.get());
  }

  void TearSide(StackSide& side) {
    side.ui.reset();
    if (side.stack) {
      side.stack->AbortCallMediaForShutdown();
      side.stack->Shutdown();
    }
    side.stack.reset();
    side.transport.reset();
    side.dial.reset();
    if (side.psk) {
      side.psk->ClearDek();
    }
    side.psk.reset();
    side.identity.reset();
    side.contacts.reset();
    side.store.reset();
    if (!side.data_dir.empty()) {
      std::filesystem::remove_all(side.data_dir);
    }
  }

  /** Deliver queued call-control between the two stacks; drain UI between hops. */
  void PumpWire(int rounds = 8) {
    for (int r = 0; r < rounds; ++r) {
      bool moved = false;
      while (!answer_inbox_.empty()) {
        ThreadMessage msg = answer_inbox_.front();
        answer_inbox_.pop_front();
        ASSERT_TRUE(answer_.inbound.apply_inbound_control(msg, offer_.local_identity, std::nullopt,
                                                          std::nullopt));
        moved = true;
      }
      while (!offer_inbox_.empty()) {
        ThreadMessage msg = offer_inbox_.front();
        offer_inbox_.pop_front();
        ASSERT_TRUE(offer_.inbound.apply_inbound_control(msg, answer_.local_identity, std::nullopt,
                                                         std::nullopt));
        moved = true;
      }
      AppRuntime::RunUITasks();
      if (!moved) {
        break;
      }
    }
  }

  /** Offer StartCall → Answer Accept → both InCall. Leaves call active. */
  std::string RunOfferAnswerToInCall(const std::string& thread_id) {
    auto started = offer_.ui->StartCall(thread_id, false, {answer_.local_identity});
    EXPECT_TRUE(started) << (started ? "" : started.error().message);
    if (!started) {
      return {};
    }
    const std::string call_id = started->call_id;

    auto key = offer_.stack->MediaKeys()->LoadEpochKey(call_id, 1);
    EXPECT_TRUE(key && key->has_value());
    if (!key || !key->has_value()) {
      return {};
    }
    EXPECT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_id, 1, **key));

    PumpWire();
    auto pending = answer_.ui->TopPendingInvite();
    EXPECT_TRUE(pending && pending->has_value()) << "invite should land on answerer";
    if (!pending || !pending->has_value()) {
      return {};
    }

    answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
    answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_id);

    DrainUntil([&]() {
      PumpWire();
      const bool answer_ok =
          answer_.stack->MediaEngine() && answer_.stack->MediaEngine()->IsActive() &&
          answer_.ui->Phase() == CallPhase::InCall;
      const bool offer_ok = offer_.stack->MediaEngine() && offer_.stack->MediaEngine()->IsActive() &&
                            offer_.ui->Phase() == CallPhase::InCall;
      return answer_ok && offer_ok;
    });
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::InCall);
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::InCall);
    return call_id;
  }

  void FinishAnswerLeaveExpectBothIdle(const std::string& call_id) {
    answer_.ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    // Product: answer Leave → CallLeave/CallEnded on wire → offer EndCallLocal → RemoteEnded.
    // Do not chrome-heal with offer LeaveClicked — that masked the missing RemoteEnded wire.
    DrainUntil([&]() {
      PumpWire();
      const bool answer_idle = answer_.ui->Phase() == CallPhase::Idle && !answer_.stack->HasActiveLocalCall();
      const bool offer_idle = offer_.ui->Phase() == CallPhase::Idle && !offer_.stack->HasActiveLocalCall();
      return answer_idle && offer_idle;
    });
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle)
        << "offerer must Idle on remote Leave without local LeaveClicked";
    EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
    EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
    offer_inbox_.clear();
    answer_inbox_.clear();
  }

  void FinishOfferLeaveExpectBothIdle(const std::string& call_id) {
    offer_.ui->Apply(CallLifecycleEvent::LeaveClicked, call_id);
    // Symmetric: offerer Leave must Idle answerer without answer LeaveClicked.
    DrainUntil([&]() {
      PumpWire();
      const bool answer_idle = answer_.ui->Phase() == CallPhase::Idle && !answer_.stack->HasActiveLocalCall();
      const bool offer_idle = offer_.ui->Phase() == CallPhase::Idle && !offer_.stack->HasActiveLocalCall();
      return answer_idle && offer_idle;
    });
    EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
    EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle)
        << "answerer must Idle on remote Leave without local LeaveClicked";
    EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
    EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
    offer_inbox_.clear();
    answer_inbox_.clear();
  }

  /** Offer StartCall → Answer Accept → both InCall → Answer Leave → both Idle (no offer heal). */
  std::string RunOfferAnswerInCallLeave(const std::string& thread_id) {
    const std::string call_id = RunOfferAnswerToInCall(thread_id);
    if (call_id.empty()) {
      return {};
    }
    FinishAnswerLeaveExpectBothIdle(call_id);
    return call_id;
  }

  std::unique_ptr<MeshControlPool> mesh_control_;
  StackSide offer_;
  StackSide answer_;
  std::deque<ThreadMessage> offer_inbox_;
  std::deque<ThreadMessage> answer_inbox_;
};

TEST_F(CallDualStackComposeTest, OfferInviteAcceptInCallLeave) {
  // Product-shaped wire: Offer StartCall → Invite → Answer Accept → CallAccept → both media → Leave.
  Thread thread;
  // Windows: thread id is a directory name under threads/ — no ':' (illegal path char).
  thread.id = "thread-dual-dm";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_id = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_id.empty());
  EXPECT_GE(answer_.transport->connect_async_calls, 1);
}

TEST_F(CallDualStackComposeTest, OfferLeaveClearsAnswererIdle) {
  // CALLS remote end (symmetric): offerer Leave → answerer Idle without answer LeaveClicked.
  Thread thread;
  thread.id = "thread-dual-offer-leave";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_id = RunOfferAnswerToInCall(thread.id);
  ASSERT_FALSE(call_id.empty());
  FinishOfferLeaveExpectBothIdle(call_id);
}

TEST_F(CallDualStackComposeTest, OfferAnswerKCycleTeardown) {
  // B-TEARDOWN: Leave→Idle→second Invite→InCall on dual CallStack (no stuck listen/media).
  Thread thread;
  thread.id = "thread-dual-kcycle";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const int connect_before = answer_.transport->connect_async_calls;
  const std::string call_1 = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_1.empty());
  EXPECT_FALSE(offer_.stack->WantEphemeralListen());
  EXPECT_FALSE(answer_.stack->WantEphemeralListen());
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);

  const std::string call_2 = RunOfferAnswerInCallLeave(thread.id);
  ASSERT_FALSE(call_2.empty());
  EXPECT_NE(call_1, call_2);
  EXPECT_GT(answer_.transport->connect_async_calls, connect_before);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
  EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
  EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
}

TEST_F(CallDualStackComposeTest, OfferInviteAnswerDeclineClearsOfferer) {
  // CALLS: answer Decline → CallDecline on wire → offerer Idle (no LeaveClicked / TTL).
  Thread thread;
  thread.id = "thread-dual-decline";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  auto started = offer_.ui->StartCall(thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started) << started.error().message;
  const std::string call_id = started->call_id;
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::OutboundCalling);

  PumpWire();
  auto pending = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, call_id);

  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_id);
  answer_.ui->Apply(CallLifecycleEvent::DeclineClicked, call_id);

  DrainUntil([&]() {
    PumpWire();
    return answer_.ui->Phase() == CallPhase::Idle && offer_.ui->Phase() == CallPhase::Idle &&
           !offer_.stack->HasActiveLocalCall();
  });
  EXPECT_EQ(answer_.ui->Phase(), CallPhase::Idle);
  EXPECT_EQ(offer_.ui->Phase(), CallPhase::Idle)
      << "offerer must Idle on remote Decline without local LeaveClicked";
  EXPECT_FALSE(offer_.stack->HasActiveLocalCall());
  EXPECT_FALSE(answer_.stack->HasActiveLocalCall());
}

TEST_F(CallDualStackComposeTest, AcceptSecondInviteEndsPriorActiveCall) {
  // B-CONFLICT: Accept call B while InCall on A ends A (LeaveCallIfActiveExcept) across stacks.
  Thread thread;
  thread.id = "thread-dual-conflict";
  thread.kind = ThreadKind::Direct;
  thread.title = "Answer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(offer_.store->UpsertThread(thread));

  const std::string call_a = RunOfferAnswerToInCall(thread.id);
  ASSERT_FALSE(call_a.empty());
  auto active_a = answer_.ui->ActiveLocalCall();
  ASSERT_TRUE(active_a && active_a->has_value());
  EXPECT_EQ((*active_a)->call_id, call_a);

  // Second outbound invite while still InCall on A.
  auto started_b = offer_.ui->StartCall(thread.id, false, {answer_.local_identity});
  ASSERT_TRUE(started_b) << started_b.error().message;
  const std::string call_b = started_b->call_id;
  EXPECT_NE(call_a, call_b);
  // Offerer LeaveCallIfActiveExcept ends A when starting B.
  {
    auto offer_active = offer_.ui->ActiveLocalCall();
    ASSERT_TRUE(offer_active && offer_active->has_value());
    EXPECT_EQ((*offer_active)->call_id, call_b);
  }

  auto key_b = offer_.stack->MediaKeys()->LoadEpochKey(call_b, 1);
  ASSERT_TRUE(key_b && key_b->has_value());
  ASSERT_TRUE(answer_.stack->MediaKeys()->PutEpochKey(call_b, 1, **key_b));

  PumpWire();
  // Answer may still show A active until Accept B; pending B should be visible.
  auto pending_b = answer_.ui->TopPendingInvite();
  ASSERT_TRUE(pending_b && pending_b->has_value());
  EXPECT_EQ((*pending_b)->call_id, call_b);

  answer_.ui->Apply(CallLifecycleEvent::InviteSeen, call_b);
  answer_.ui->Apply(CallLifecycleEvent::AcceptClicked, call_b);

  DrainUntil([&]() {
    PumpWire();
    auto active = answer_.ui->ActiveLocalCall();
    return active && active->has_value() && (*active)->call_id == call_b &&
           answer_.ui->Phase() == CallPhase::InCall && offer_.ui->Phase() == CallPhase::InCall;
  });
  auto active_b = answer_.ui->ActiveLocalCall();
  ASSERT_TRUE(active_b && active_b->has_value());
  EXPECT_EQ((*active_b)->call_id, call_b);

  // Prior call A must be ended on both sides.
  auto offer_a = offer_.stack->Calls()->ActiveLocalCall();
  ASSERT_TRUE(offer_a);
  if (offer_a->has_value()) {
    EXPECT_NE((*offer_a)->call_id, call_a);
  }
  auto answer_sessions = answer_.stack->Calls();
  ASSERT_TRUE(answer_sessions);
  // ActiveLocalCall is only the joined call — A must not be the active one.
  EXPECT_EQ((*active_b)->call_id, call_b);

  FinishAnswerLeaveExpectBothIdle(call_b);
}

} // namespace
} // namespace pbr
