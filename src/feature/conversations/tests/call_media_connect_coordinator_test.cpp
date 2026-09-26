#include "feature/calls/CallMediaConnectCoordinator.h"

#include "feature/calls/CallTopologyRelayDeps.h"
#include "feature/calls/PeerReachCoordinator.h"
#include "foundation/runtime/AppRuntime.h"

#include <chrono>
#include <functional>
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

class FakeDialRegistry final : public IDialRegistry {
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
  void ClearCallMediaCircuitHop(const std::string& /*peer*/) override {}

  mutable std::mutex mu;
  std::unordered_set<std::string> connected;
  int drop_calls = 0;
  int abort_calls = 0;
};

/** Punch/circuit that re-establishes the link, so a fresh-link redial can succeed. */
class FakeCircuitReach final : public ICircuitHopReach {
public:
  explicit FakeCircuitReach(FakeDialRegistry& dial) : dial_(dial) {}
  Roe<void> TryEnsureHopReachable(const std::string& /*hop*/) override { return {}; }
  Roe<void> TryEnsureCallMediaReachable(const std::string& /*peer*/) override { return {}; }
  void TryEnsureCallMediaReachableAsync(const std::string& peer_key, std::function<void(Roe<void>)> on_done,
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
  void SetInboundHandler(std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)>) override {}
  void ClearInboundHandler() override {}
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
                    std::function<void(Roe<void>)> on_done, int /*timeout_ms*/) override {
    ++connect_calls;
    if (hang_first_n > 0) {
      --hang_first_n;
      pending = std::move(on_done);
      return;
    }
    if (fail_first_n > 0) {
      --fail_first_n;
      on_done(Error(fail_message));
      if (after_fail) {
        after_fail(connect_calls);
      }
      return;
    }
    active = true;
    active_params = params;
    on_done({});
  }
  Roe<void> Connect(const CallMediaDirectConnectParams&, CallMediaDirectCallbacks, int) override { return {}; }
  Roe<void> SendAudio(const std::vector<uint8_t>&, uint32_t, uint8_t) override { return {}; }
  Roe<void> SendMedia(uint8_t, const std::vector<uint8_t>&, uint32_t, uint8_t) override { return {}; }

  bool active = false;
  CallMediaDirectConnectParams active_params;
  int connect_calls = 0;
  int detach_calls = 0;
  int fail_first_n = 0;
  int hang_first_n = 0;
  std::string fail_message = "amp call-media: hello rejected";
  std::function<void(Roe<void>)> pending;
  /** Runs on the UI thread right after a failed completion was delivered (attempt number). */
  std::function<void(int attempt)> after_fail;
};

class CallMediaConnectCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    AppRuntime::InitializeUI();
    dial_ = std::make_unique<FakeDialRegistry>();
    circuit_ = std::make_unique<FakeCircuitReach>(*dial_);
    reach_ = std::make_unique<PeerReachCoordinator>(dial_.get(), circuit_.get());
    reach_->SetDialBudgetMsForTest(200);
    transport_ = std::make_unique<FakeTransport>();
    connect_ = std::make_unique<CallMediaConnectCoordinator>(*transport_, *reach_);
    dial_->connected.insert(kPeer);
  }

  void TearDown() override {
    connect_->Shutdown();
    reach_->CancelAll();
    AppRuntime::Shutdown();
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
    h.on_link_ready = [this](PeerLinkKind kind) { last_kind_ = kind; };
    h.on_finished = [this](Roe<void> r) {
      ++finished_;
      result_ = std::move(r);
    };
    return h;
  }

  /** Pump UI until `done` or the budget runs out. */
  static bool PumpUntil(const std::function<bool()>& done, std::chrono::milliseconds budget) {
    const auto until = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < until) {
      AppRuntime::RunUITasks();
      if (done()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    AppRuntime::RunUITasks();
    return done();
  }

  static void PumpFor(std::chrono::milliseconds span) {
    (void)PumpUntil([] { return false; }, span);
  }

  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCircuitReach> circuit_;
  std::unique_ptr<PeerReachCoordinator> reach_;
  std::unique_ptr<FakeTransport> transport_;
  std::unique_ptr<CallMediaConnectCoordinator> connect_;
  int before_attempts_ = 0;
  int finished_ = 0;
  std::optional<Roe<void>> result_;
  PeerLinkKind last_kind_ = PeerLinkKind::Unknown;
};

TEST_F(CallMediaConnectCoordinatorTest, ConnectsOnFirstAttempt) {
  connect_->Start(Request(), Hooks());
  EXPECT_TRUE(connect_->InFlight());
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(5)));
  ASSERT_TRUE(result_ && *result_);
  EXPECT_EQ(finished_, 1);
  EXPECT_EQ(transport_->connect_calls, 1);
  EXPECT_EQ(before_attempts_, 1);
  EXPECT_EQ(last_kind_, PeerLinkKind::Direct);
  EXPECT_FALSE(connect_->InFlight());
}

// B39: an attempt that failed on a reused "connected" link makes the next reach drop it and redial.
TEST_F(CallMediaConnectCoordinatorTest, FailureOnReusedLinkRedialsFresh) {
  transport_->fail_first_n = 1;
  connect_->Start(Request(), Hooks());
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
  connect_->Start(Request(), Hooks());
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(10)));
  ASSERT_TRUE(result_ && *result_);
  EXPECT_EQ(transport_->connect_calls, 2);
  EXPECT_GE(transport_->detach_calls, 1) << "watchdog detaches the stuck attempt";
  // The hung attempt completing late must not finish the sequence a second time.
  if (transport_->pending) {
    transport_->pending(Error("late"));
  }
  PumpFor(std::chrono::milliseconds(100));
  EXPECT_EQ(finished_, 1);
}

TEST_F(CallMediaConnectCoordinatorTest, GivesUpAfterAllAttempts) {
  transport_->fail_first_n = 100;
  connect_->Start(Request(), Hooks());
  ASSERT_TRUE(PumpUntil([&] { return finished_ > 0; }, std::chrono::seconds(20)));
  ASSERT_TRUE(result_);
  EXPECT_FALSE(*result_);
  EXPECT_EQ(transport_->connect_calls, 5);
  EXPECT_FALSE(connect_->InFlight());
}

// THREADING.md Cancel / Abort: abort clears InFlight itself and completes nothing.
TEST_F(CallMediaConnectCoordinatorTest, AbortClearsInFlightWithoutFinishing) {
  connect_->SetAttemptTimeoutMsForTest(100);
  transport_->hang_first_n = 1;
  connect_->Start(Request(), Hooks());
  ASSERT_TRUE(PumpUntil([&] { return transport_->connect_calls > 0; }, std::chrono::seconds(5)));
  connect_->Abort();
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
  // The last attempt's failure posts the give-up; a restart / stop lands before it runs.
  transport_->after_fail = [this](int attempt) {
    if (attempt == 5) {
      EXPECT_FALSE(connect_->InFlight()) << "sequence already gave up";
      connect_->Abort();
    }
  };
  connect_->Start(Request(), Hooks());
  ASSERT_TRUE(PumpUntil([&] { return transport_->connect_calls >= 5; }, std::chrono::seconds(20)));
  PumpFor(std::chrono::milliseconds(200));
  EXPECT_EQ(finished_, 0);
}

TEST_F(CallMediaConnectCoordinatorTest, ShutdownRejectsStart) {
  connect_->Shutdown();
  std::string failure;
  auto req = Request();
  req.callbacks.on_failed = [&failure](const std::string& why) { failure = why; };
  connect_->Start(std::move(req), Hooks());
  EXPECT_EQ(failure, "shutdown in progress");
  EXPECT_FALSE(connect_->InFlight());
  EXPECT_EQ(transport_->connect_calls, 0);
}

} // namespace
} // namespace pbr
