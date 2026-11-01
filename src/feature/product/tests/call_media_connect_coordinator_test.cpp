#include "feature/calls/CallMediaConnectCoordinator.h"

#include "common/Diag.h"
#include "foundation/diagnostics/DiagnosticsSelfCheck.h"
#include "foundation/diagnostics/LogFile.h"

#include "feature/calls/CallTopologyRelayDeps.h"
#include "mesh/reach/PeerReachCoordinator.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "foundation/runtime/ProductOwners.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include "feature/product/tests/call_media_inbound_fake.h"
#include "feature/product/tests/calls_loopback_outbox.h"
#include "feature/product/tests/calls_on_owner.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

namespace pbr {
namespace {

constexpr const char* kPeer = "12D3KooWConnectPeer";

class FakeDialRegistry final : public mesh::IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& /*peer*/, const std::string& /*ma*/) override { return {}; }
  bool IsDialable(const std::string& /*peer*/) const override { return false; }
  bool IsConnected(const std::string& peer_key) const override {
    std::lock_guard lock(mu);
    return connected.count(peer_key) > 0;
  }
  std::optional<std::string> PreferredMultiaddr(const std::string& /*peer*/) const override {
    return std::nullopt;
  }
  void ClearDialBackoff(const std::string& /*peer*/) override {}
  void AbortInflightDial(const std::string& /*peer*/) override { ++abort_calls; }
  void DropLink(const std::string& peer_key) override {
    std::lock_guard lock(mu);
    ++drop_calls;
    connected.erase(peer_key);
  }
  void ClearPeerCircuitHop(const std::string& /*peer*/) override {}

  mutable std::mutex mu;
  std::unordered_set<std::string> connected;
  int drop_calls = 0;
  int abort_calls = 0;
};

/** Punch/circuit that re-establishes the link, so a fresh-link redial can succeed. */
class FakeCircuitReach final : public mesh::ICircuitHopReach {
public:
  explicit FakeCircuitReach(FakeDialRegistry& dial) : dial_(dial) {}
  mesh::CircuitReachRoe TryEnsureHopReachable(const std::string& /*hop*/) override { return {}; }
  mesh::CircuitReachRoe TryEnsurePeerReachable(const std::string& /*peer*/) override { return {}; }
  void TryEnsurePeerReachableAsync(const std::string& peer_key, mesh::CircuitReachDone on_done,
                                        bool /*allow_circuit*/) override {
    {
      std::lock_guard lock(dial_.mu);
      dial_.connected.insert(peer_key);
    }
    on_done({});
  }

private:
  FakeDialRegistry& dial_;
};

class FakeTransport final : public ICallMediaTransport {
public:
  void Start() override {}
  void Stop() override {}
  void SetInboundHandler(CallMediaInboundHandler handler) override { inbound.Set(std::move(handler)); }
  void ClearInboundHandler() override { inbound.Clear(); }
  /** Deliver a hello the way the transport's IO hop does and wait for the owner's answer. */
  bool DeliverHello(CallMediaDirectConnectParams& params, CallMediaDirectCallbacks& cbs) {
    return inbound.DeliverAndWait(params, cbs);
  }
  bool IsActive() const override { return active; }
  CallMediaDirectConnectParams ActiveParams() const override { return active_params; }
  CallMediaSessionPhase Phase() const override {
    return active ? CallMediaSessionPhase::MediaReady : CallMediaSessionPhase::Idle;
  }
  void Detach() override {
    active = false;
    ++detach_calls;
  }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks /*cbs*/,
                    std::function<void(CallMediaRoe)> on_done, int /*timeout_ms*/) override {
    ++connect_calls;
    if (hang_first_n > 0) {
      --hang_first_n;
      pending = std::move(on_done);
      return;
    }
    if (fail_first_n > 0) {
      --fail_first_n;
      on_done(CallMediaFailure::Of(fail_code, fail_message));
      if (after_fail) {
        after_fail(connect_calls);
      }
      return;
    }
    active = true;
    active_params = params;
    on_done({});
  }
  CallMediaRoe Connect(const CallMediaDirectConnectParams&, CallMediaDirectCallbacks, int) override { return {}; }
  Roe<void> SendAudio(const std::vector<uint8_t>&, uint32_t, uint8_t) override { return {}; }
  Roe<void> SendMedia(uint8_t, const std::vector<uint8_t>&, uint32_t, uint8_t) override { return {}; }

  bool active = false;
  CallMediaDirectConnectParams active_params;
  int connect_calls = 0;
  int detach_calls = 0;
  int fail_first_n = 0;
  int hang_first_n = 0;
  std::string fail_message = "amp call-media: hello rejected";
  CallMediaErr fail_code = CallMediaErr::HelloRejected;
  std::function<void(CallMediaRoe)> pending;
  test::InboundHelloFake inbound;
  /** Runs on the UI thread right after a failed completion was delivered (attempt number). */
  std::function<void(int attempt)> after_fail;
};

class CallMediaConnectCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(ManualOwnerRuntimeConfig());
    AppRuntime::InitializeUI();
    dial_ = std::make_unique<FakeDialRegistry>();
    circuit_ = std::make_unique<FakeCircuitReach>(*dial_);
    reach_ = std::make_unique<mesh::PeerReachCoordinator>(
        mesh::PeerReachPorts{dial_.get(), circuit_.get(), AppRuntime::MakeLaneForTesting()});
    reach_->SetDialBudgetMsForTest(200);
    transport_ = std::make_unique<FakeTransport>();
    connect_ = std::make_unique<CallMediaConnectCoordinator>(*transport_, *reach_);
    events_ = Loopback(connect_.get());
    dial_->connected.insert(kPeer);
  }

  void TearDown() override {
    OnCallsOwner([&] { connect_->Shutdown(); });
    reach_->CancelAll();
    AppRuntime::Shutdown();
    events_.reset();
    connect_.reset();
    transport_.reset();
    reach_.reset();
    circuit_.reset();
    dial_.reset();
    AppRuntime::ShutdownUI();
  }

  CallMediaConnectRequest Request() const {
    CallMediaConnectRequest r;
    r.params.call_id = "call:connect";
    r.params.peer_key = kPeer;
    r.params.media_key = ByteVector(32, 0x11);
    r.params.offerer = true;
    r.reach.keys = {kPeer};
    return r;
  }

  CallMediaConnectHooks Hooks() {
    CallMediaConnectHooks h;
    h.before_attempt = [this](const CallMediaDirectConnectParams&) { ++before_attempts_; };
    h.on_link_ready = [this](mesh::PeerLinkKind kind) { last_kind_ = kind; };
    h.on_finished = [this](CallMediaRoe r) {
      ++finished_;
      result_ = std::move(r);
    };
    return h;
  }

  /** The fixture plays the bridge: `coordinator`'s events come back to it on the calls owner. */
  static std::unique_ptr<CallsLoopbackOutbox<ConnectEvent>> Loopback(CallMediaConnectCoordinator* coordinator) {
    auto loop = std::make_unique<CallsLoopbackOutbox<ConnectEvent>>(
        [coordinator](ConnectEvent& event) { coordinator->Handle(event); });
    coordinator->SetOutbox(loop->Get());
    return loop;
  }

  /** Pump UI and the calls owner until `done` or the budget runs out. */
  static bool PumpUntil(const std::function<bool()>& done, std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < until) {
      AppRuntime::RunUIAndOwnerTasks();
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    AppRuntime::RunUIAndOwnerTasks();
    return done();
  }

  static void PumpFor(std::chrono::milliseconds span) {
    (void)PumpUntil([] { return false; }, span);
  }

  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCircuitReach> circuit_;
  std::unique_ptr<mesh::PeerReachCoordinator> reach_;
  std::unique_ptr<FakeTransport> transport_;
  std::unique_ptr<CallMediaConnectCoordinator> connect_;
  /** Reset before `connect_`: its queued events point at the coordinator. */
  std::unique_ptr<CallsLoopbackOutbox<ConnectEvent>> events_;
  int before_attempts_ = 0;
  int finished_ = 0;
  std::optional<CallMediaRoe> result_;
  mesh::PeerLinkKind last_kind_ = mesh::PeerLinkKind::Unknown;
};

TEST_F(CallMediaConnectCoordinatorTest, ConnectsOnFirstAttempt) {
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  EXPECT_TRUE(connect_->InFlight());
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(5)));
  ASSERT_TRUE(result_ && *result_);
  EXPECT_EQ(finished_, 1);
  EXPECT_EQ(transport_->connect_calls, 1);
  EXPECT_EQ(before_attempts_, 1);
  EXPECT_EQ(last_kind_, mesh::PeerLinkKind::Direct);
  EXPECT_FALSE(connect_->InFlight());
}

// B39: an attempt that failed on a reused "connected" link makes the next reach drop it and redial.
TEST_F(CallMediaConnectCoordinatorTest, FailureOnReusedLinkRedialsFresh) {
  transport_->fail_first_n = 1;
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(10)));
  ASSERT_TRUE(result_ && *result_) << result_->error().message;
  EXPECT_EQ(transport_->connect_calls, 2);
  EXPECT_EQ(dial_->drop_calls, 1) << "second reach must drop the stale link";
  EXPECT_EQ(before_attempts_, 2) << "offerer hook runs per attempt";
}

// B42: the watchdog fails only the stuck attempt; the next attempt still runs.
TEST_F(CallMediaConnectCoordinatorTest, WatchdogFailsOnlyTheAttempt) {
  connect_->SetAttemptTimeoutMsForTest(100);
  transport_->hang_first_n = 1;
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(10)));
  ASSERT_TRUE(result_ && *result_);
  EXPECT_EQ(transport_->connect_calls, 2);
  EXPECT_GE(transport_->detach_calls, 1) << "watchdog detaches the stuck attempt";
  // The hung attempt completing late must not finish the sequence a second time.
  if (transport_->pending) {
    transport_->pending(CallMediaFailure::Of(CallMediaErr::Timeout, "late"));
  }
  PumpFor(std::chrono::milliseconds(100));
  EXPECT_EQ(finished_, 1);
}

TEST_F(CallMediaConnectCoordinatorTest, GivesUpAfterAllAttempts) {
  transport_->fail_first_n = 100;
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(20)));
  ASSERT_TRUE(result_);
  ASSERT_FALSE(*result_);
  EXPECT_EQ(result_->error().GetCode(), transport_->fail_code) << "the last attempt's code, not its text";
  EXPECT_EQ(transport_->connect_calls, 5);
  EXPECT_FALSE(connect_->InFlight());
}

// --- the connect sequence's lines of the Diag channel (docs/contracts/DIAGNOSTICS.md) ------------------------

/** A circuit step that finds the target parked on no relay, as the field reports of #416 read. */
class RefusingCircuitReach final : public mesh::ICircuitHopReach {
public:
  mesh::CircuitReachRoe TryEnsureHopReachable(const std::string&) override { return {}; }
  mesh::CircuitReachRoe TryEnsurePeerReachable(const std::string&) override { return {}; }
  void TryEnsurePeerReachableAsync(const std::string&, mesh::CircuitReachDone on_done, bool) override {
    on_done(mesh::CircuitReachFailure::Of(
        mesh::CircuitReachErr::TargetNotReserved,
        "circuit hop reach failed [tunnel: circuit-relay: refused [relay: circuit target peer endpoint not registered]]"));
  }
};

class CallConnectDiagSequenceTest : public CallMediaConnectCoordinatorTest {
protected:
  void SetUp() override {
    CallMediaConnectCoordinatorTest::SetUp();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("pp-browser-call-diag-") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
    std::filesystem::create_directories(dir_ / "logs", ec);
    diag_path_ = LogFile::InstallDiag(LogFile::DefaultPath(dir_.string()));
    CallConnectEnv env;
    env.local.v4_private = 1;
    env.local.v6_global = 2;
    env.attachment_known = true;
    env.attachment.online = true;
    env.attachment.cellular = true;
    env.relays_surface = 9;
    env.relays_connected = 4;
    env.relays_held_down = 2;
    env.peer_state = mesh::PeerReachState::Down;
    env.peer_fail = mesh::PeerReachFailure::NoAnswer;
    env.peer_fails_in_row = 3;
    env.peer_pref_family = 6;
    connect_->SetDiagProbe([env](const std::string&) { return env; });
  }
  void TearDown() override {
    CallMediaConnectCoordinatorTest::TearDown();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  CallMediaConnectRequest RequestFor(const std::string& call_id) const {
    CallMediaConnectRequest r = Request();
    r.params.call_id = call_id;
    return r;
  }

  /** The event lines of this run, each without its timestamp prefix. */
  std::vector<std::string> Lines() const {
    std::ifstream in(diag_path_);
    std::vector<std::string> out;
    for (std::string line; std::getline(in, line);) {
      const auto at = line.find("[Diag] ");
      if (at != std::string::npos) {
        out.push_back(line.substr(at + 7));
      }
    }
    return out;
  }

  std::filesystem::path dir_;
  std::string diag_path_;
};

TEST_F(CallConnectDiagSequenceTest, SuccessWritesNetReachAndResult) {
  OnCallsOwner([&] { connect_->Start(RequestFor("call:5ca1ab0100000000"), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(5)));
  ASSERT_TRUE(result_ && *result_);
  const std::vector<std::string> lines = Lines();
  ASSERT_EQ(lines.size(), 3u) << ::testing::PrintToString(lines);
  EXPECT_EQ(lines[0],
            "event=call.net call=5ca1ab01 role=caller known=1 online=1 cellular=1 expensive=0 v4_private=1 "
            "v4_public=0 v6_global=2 v6_other=0 relays=9 relays_up=4 relays_down=2 peer_state=down "
            "peer_fail=no_answer peer_fails=3");
  EXPECT_EQ(lines[1].rfind("event=call.reach call=5ca1ab01 role=caller attempt=1 ok=1 kind=direct family=6 reused=1 ms=", 0), 0u)
      << lines[1];
  EXPECT_EQ(lines[2].rfind("event=call.result call=5ca1ab01 role=caller ok=1 attempts=1 ms=", 0), 0u) << lines[2];
  EXPECT_NE(lines[2].find(" kind=direct family=6"), std::string::npos) << lines[2];
  EXPECT_EQ(lines[2].find("err="), std::string::npos) << "a success carries no failure keys";
}

TEST_F(CallConnectDiagSequenceTest, GiveUpWritesOneReachLinePerAttemptAndTheReason) {
  transport_->fail_first_n = 100;
  transport_->fail_code = CallMediaErr::Timeout;
  OnCallsOwner([&] { connect_->Start(RequestFor("call:5ca1ab0200000000"), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(20)));
  ASSERT_TRUE(result_ && !*result_);
  const std::vector<std::string> lines = Lines();
  ASSERT_EQ(lines.size(), 7u) << ::testing::PrintToString(lines);  // net, five attempts, result
  EXPECT_EQ(lines.front().rfind("event=call.net ", 0), 0u);
  for (size_t i = 1; i <= 5; ++i) {
    EXPECT_EQ(lines[i].rfind("event=call.reach call=5ca1ab02 role=caller attempt=" + std::to_string(i) + " ok=1", 0), 0u)
        << lines[i];
  }
  EXPECT_EQ(lines.back().rfind("event=call.result call=5ca1ab02 role=caller ok=0 attempts=5 ms=", 0), 0u) << lines.back();
  EXPECT_NE(lines.back().find(" err=timeout reach_err=none why=none relays=9 relays_up=4 peer_state=down "
                              "peer_fail=no_answer peer_fails=3"),
            std::string::npos)
      << lines.back();
}

TEST_F(CallConnectDiagSequenceTest, ARefusedCircuitIsNamedWithoutItsText) {
  dial_->connected.clear();
  RefusingCircuitReach refusing;
  reach_->SetDeps(mesh::PeerReachPorts{dial_.get(), &refusing, AppRuntime::MakeLaneForTesting()});
  OnCallsOwner([&] { connect_->Start(RequestFor("call:5ca1ab0300000000"), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(20)));
  ASSERT_TRUE(result_ && !*result_);
  const std::vector<std::string> lines = Lines();
  ASSERT_GE(lines.size(), 3u) << ::testing::PrintToString(lines);
  EXPECT_NE(lines[1].find("event=call.reach call=5ca1ab03 role=caller attempt=1 ok=0 err="), std::string::npos) << lines[1];
  EXPECT_NE(lines.back().find("ok=0 attempts=1"), std::string::npos) << lines.back();
  EXPECT_NE(lines.back().find(" err=unreachable reach_err="), std::string::npos) << lines.back();
  for (const std::string& line : lines) {
    EXPECT_EQ(line.find("endpoint"), std::string::npos) << "no message text in a line: " << line;
  }
}

// Whatever the sequence wrote must pass the gate every send goes through, or no send happens at all.
TEST_F(CallConnectDiagSequenceTest, TheLinesPassTheBundleSelfCheck) {
  OnCallsOwner([&] { connect_->Start(RequestFor("call:5ca1ab0400000000"), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(5)));
  std::ifstream in(diag_path_);
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  ASSERT_FALSE(text.empty());
  EXPECT_EQ(SelfCheck(text, {}), std::nullopt) << text;
}

// A retry loop cannot flood the file: each event is capped per call.
TEST_F(CallConnectDiagSequenceTest, ARestartLoopIsBoundedPerCall) {
  for (int i = 0; i < 20; ++i) {
    finished_ = 0;
    OnCallsOwner([&] { connect_->Start(RequestFor("call:5ca1ab0500000000"), Hooks()); });
    ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(5)));
    transport_->active = false;
  }
  size_t nets = 0;
  for (const std::string& line : Lines()) {
    nets += line.rfind("event=call.net ", 0) == 0 ? 1 : 0;
  }
  EXPECT_EQ(nets, static_cast<size_t>(CallDiagBudget::kPerEventPerCall)) << ::testing::PrintToString(Lines());
}

// A peer reach that found no link ends the sequence with this layer's code (the reach's chain kept).
TEST(CallMediaConnectWrapTest, AReachFailureIsWrappedAsCallMediaCodes) {
  using ReachErr = mesh::PeerReachCoordinator::Err;
  const auto wrap = [](ReachErr code) {
    return CallMediaConnectCoordinator::WrapReachFailure(mesh::PeerReachCoordinator::Failure::Of(code, "why"));
  };
  EXPECT_EQ(wrap(ReachErr::NoEndpoint).GetCode(), CallMediaErr::Unreachable);
  EXPECT_EQ(wrap(ReachErr::NotConnected).GetCode(), CallMediaErr::Unreachable);
  EXPECT_EQ(wrap(ReachErr::CircuitFailed).GetCode(), CallMediaErr::Unreachable);
  EXPECT_EQ(wrap(ReachErr::NoAnswer).GetCode(), CallMediaErr::Timeout);
  EXPECT_EQ(wrap(ReachErr::TransportFailed).GetCode(), CallMediaErr::StaleLink);
  EXPECT_EQ(wrap(ReachErr::Cancelled).GetCode(), CallMediaErr::Aborted);
  EXPECT_NE(wrap(ReachErr::NoAnswer).message.find("why"), std::string::npos);
}

// THREADING.md Cancel / Abort: abort clears InFlight itself and completes nothing.
TEST_F(CallMediaConnectCoordinatorTest, AbortClearsInFlightWithoutFinishing) {
  connect_->SetAttemptTimeoutMsForTest(100);
  transport_->hang_first_n = 1;
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return transport_->connect_calls > 0; }, std::chrono::seconds(5)));
  OnCallsOwner([&] { connect_->Abort(); });
  EXPECT_FALSE(connect_->InFlight());
  if (transport_->pending) {
    transport_->pending({});
  }
  PumpFor(std::chrono::milliseconds(1300));  // past the watchdog
  EXPECT_EQ(finished_, 0);
  EXPECT_EQ(transport_->connect_calls, 1) << "no retry after abort";
}

// A give-up posted before a restart must not reach the handler (it would fail the new session).
TEST_F(CallMediaConnectCoordinatorTest, StaleGiveUpIsNotDelivered) {
  transport_->fail_first_n = 100;
  // The last attempt's failure reports the give-up as an event; a restart / stop lands before it
  // is handled. Queued behind the attempt's result, so it runs between the give-up and its delivery.
  transport_->after_fail = [this](int attempt) {
    if (attempt == 5) {
      AppRuntime::PostTo(kMediaSessionsOwner, [this]() {
        EXPECT_FALSE(connect_->InFlight()) << "sequence already gave up";
        connect_->Abort();
      });
    }
  };
  OnCallsOwner([&] { connect_->Start(Request(), Hooks()); });
  ASSERT_TRUE(PumpUntil([&] { return transport_->connect_calls >= 5; }, std::chrono::seconds(20)));
  PumpFor(std::chrono::milliseconds(200));
  EXPECT_EQ(finished_, 0);
}

TEST_F(CallMediaConnectCoordinatorTest, ShutdownRejectsStart) {
  OnCallsOwner([&] { connect_->Shutdown(); });
  std::string failure;
  auto req = Request();
  req.callbacks.on_failed = [&failure](const CallMediaFailure& why) { failure = why.message; };
  OnCallsOwner([&] { connect_->Start(std::move(req), Hooks()); });
  EXPECT_EQ(failure, "shutdown in progress");
  EXPECT_FALSE(connect_->InFlight());
  EXPECT_EQ(transport_->connect_calls, 0);
}

// --- Inbound ---------------------------------------------------------------------------

/** Ports backed by test state (read on the calls owner). */
struct InboundFixture {
  std::atomic<bool> session_open{true};
  std::atomic<bool> key_stored{false};
  std::atomic<int> key_requests{0};
  std::atomic<int> accepted{0};
  std::mutex mu;
  std::string accepted_peer;

  CallMediaInboundPorts Ports() {
    CallMediaInboundPorts ports;
    ports.session_open = [this](const std::string&) { return session_open.load(); };
    ports.load_key = [this](const std::string&, uint32_t) -> std::optional<ByteVector> {
      if (key_stored.load()) {
        return ByteVector(32, 0x22);
      }
      return std::nullopt;
    };
    ports.request_key = [this](const std::string&) { key_requests.fetch_add(1); };
    ports.on_accepted = [this](const CallMediaInboundHello& hello) {
      {
        std::lock_guard lock(mu);
        accepted_peer = hello.peer_id;
      }
      accepted.fetch_add(1);
      CallMediaDirectCallbacks cbs;
      cbs.on_connected = []() {};
      return cbs;
    };
    return ports;
  }
};

CallMediaDirectConnectParams InboundParams() {
  CallMediaDirectConnectParams p;
  p.call_id = "call:inbound";
  p.media_epoch = 1;
  p.peer_key = kPeer;
  p.offerer = true;
  return p;
}

TEST_F(CallMediaConnectCoordinatorTest, InboundRejectedWithoutOpenSession) {
  InboundFixture fx;
  fx.session_open = false;
  fx.key_stored = true;
  connect_->SetInboundPorts(fx.Ports());
  auto params = InboundParams();
  CallMediaDirectCallbacks cbs;
  ASSERT_TRUE(transport_->DeliverHello(params, cbs));
  EXPECT_TRUE(params.media_key.empty()) << "empty key → transport NACKs";
  EXPECT_FALSE(cbs.on_connected);
  EXPECT_EQ(fx.accepted.load(), 0);
}

TEST_F(CallMediaConnectCoordinatorTest, InboundAcceptedWhenKeyStored) {
  InboundFixture fx;
  fx.key_stored = true;
  connect_->SetInboundPorts(fx.Ports());
  auto params = InboundParams();
  CallMediaDirectCallbacks cbs;
  ASSERT_TRUE(transport_->DeliverHello(params, cbs));
  EXPECT_EQ(params.media_key.size(), 32u);
  EXPECT_TRUE(cbs.on_connected);
  EXPECT_EQ(fx.accepted.load(), 1);
  std::lock_guard lock(fx.mu);
  EXPECT_EQ(fx.accepted_peer, kPeer) << "owner gets the dialer's mesh PeerId";
}

// The offerer often dials before the relay delivers the key: the hello is parked (no thread
// waits), asking for the key, and answered as soon as it lands.
TEST_F(CallMediaConnectCoordinatorTest, InboundWaitsForKeyAndWakesOnNotify) {
  InboundFixture fx;
  connect_->SetInboundKeyWaitMsForTest(10000);
  connect_->SetInboundPorts(fx.Ports());
  auto answer = transport_->inbound.Deliver(InboundParams());
  ASSERT_TRUE(answer);
  ASSERT_TRUE(PumpUntil([&] { return fx.key_requests.load() >= 1; }, std::chrono::seconds(2)));
  EXPECT_FALSE(test::InboundHelloFake::Answered(answer)) << "parked until the key lands";
  fx.key_stored = true;
  connect_->NotifyKeyAvailable();
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_TRUE(test::InboundHelloFake::Answered(answer)) << "the notify answers at once, not at the next poll";
  EXPECT_EQ(answer->params.media_key.size(), 32u);
  EXPECT_EQ(fx.accepted.load(), 1);
}

TEST_F(CallMediaConnectCoordinatorTest, InboundKeyTimeoutRejects) {
  InboundFixture fx;
  connect_->SetInboundKeyWaitMsForTest(100);
  connect_->SetInboundPorts(fx.Ports());
  auto params = InboundParams();
  CallMediaDirectCallbacks cbs;
  ASSERT_TRUE(transport_->DeliverHello(params, cbs));
  EXPECT_TRUE(params.media_key.empty());
  EXPECT_EQ(fx.accepted.load(), 0);
  EXPECT_GE(fx.key_requests.load(), 1) << "asks for the key while waiting";
}

// THREADING.md: teardown answers (NACKs) a hello parked on the key and drops the raw-this handler.
TEST_F(CallMediaConnectCoordinatorTest, ShutdownReleasesWaitingHelloAndDropsHandler) {
  InboundFixture fx;
  connect_->SetInboundKeyWaitMsForTest(10000);
  connect_->SetInboundPorts(fx.Ports());
  auto answer = transport_->inbound.Deliver(InboundParams());
  ASSERT_TRUE(answer);
  ASSERT_TRUE(PumpUntil([&] { return fx.key_requests.load() >= 1; }, std::chrono::seconds(2)));
  OnCallsOwner([&] { connect_->Shutdown(); });
  ASSERT_TRUE(test::InboundHelloFake::Answered(answer)) << "answered by Shutdown itself";
  EXPECT_TRUE(answer->params.media_key.empty()) << "NACK";
  EXPECT_EQ(fx.accepted.load(), 0);
  auto again = InboundParams();
  CallMediaDirectCallbacks again_cbs;
  EXPECT_FALSE(transport_->DeliverHello(again, again_cbs)) << "handler cleared";
}

// CallSessionManager::AttachDirectPath builds the replacement path before destroying the old one.
TEST_F(CallMediaConnectCoordinatorTest, DestroyingOldOwnerKeepsReplacementHandler) {
  InboundFixture old_fx;
  InboundFixture new_fx;
  new_fx.key_stored = true;
  connect_->SetInboundPorts(old_fx.Ports());
  auto replacement = std::make_unique<CallMediaConnectCoordinator>(*transport_, *reach_);
  auto replacement_events = Loopback(replacement.get());
  replacement->SetInboundPorts(new_fx.Ports());
  events_.reset();
  OnCallsOwner([&] { connect_ = std::move(replacement); });  // destroys the old owner
  events_ = std::move(replacement_events);
  auto params = InboundParams();
  CallMediaDirectCallbacks cbs;
  ASSERT_TRUE(transport_->DeliverHello(params, cbs));
  EXPECT_EQ(new_fx.accepted.load(), 1);
  EXPECT_EQ(old_fx.accepted.load(), 0);
}

} // namespace
} // namespace pbr
