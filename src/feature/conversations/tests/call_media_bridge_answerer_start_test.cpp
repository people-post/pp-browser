#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallTopologyRelayDeps.h"
#include "domain/messaging/CallLifecycleTypes.h"

#include "domain/media/CallMediaEngine.h"
#include "domain/messaging/CallMediaKeyStore.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/runtime/AppRuntime.h"
#include "common/Utilities.h"

#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

ByteVector TestMediaKey() {
  ByteVector key(32);
  for (size_t i = 0; i < key.size(); ++i) {
    key[i] = static_cast<uint8_t>(0x10 + i);
  }
  return key;
}

CallDirectArmingPorts TestDirectArmingPorts(CallLifecycle* lifecycle) {
  CallDirectArmingPorts ports;
  if (!lifecycle) {
    return ports;
  }
  ports.direct_ops_allowed = [lifecycle]() { return lifecycle->AllowsDirectPath(); };
  ports.request_direct_arming = [lifecycle](const std::string& call_id) {
    if (lifecycle->AllowsDirectPath()) {
      return;
    }
    const CallPhase phase = lifecycle->Phase();
    if (phase == CallPhase::Accepting || phase == CallPhase::JoinedLocal ||
        phase == CallPhase::MediaPending || phase == CallPhase::MediaConnecting) {
      lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
    }
  };
  ports.report_progress = [lifecycle](CallDirectPlannerPhase phase, const std::string& call_id) {
    if (phase == CallDirectPlannerPhase::Live || phase == CallDirectPlannerPhase::Idle ||
        phase == CallDirectPlannerPhase::Stopping) {
      return;
    }
    if (phase == CallDirectPlannerPhase::DegradedTxOnly) {
      lifecycle->SetMediaStatus(CallMediaStatus::DegradedTxOnly, call_id);
      return;
    }
    lifecycle->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  };
  ports.on_connected = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::DirectConnected, call_id);
  };
  ports.on_connect_failed = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::ConnectFailedEvt, call_id);
  };
  ports.on_media_deferred = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaDeferred, call_id);
  };
  ports.on_media_key_ready = [lifecycle](const std::string& call_id) {
    lifecycle->Apply(CallLifecycleEvent::MediaKeyReady, call_id);
  };
  ports.arming_debug_name = [lifecycle]() { return CallMediaStatusName(lifecycle->Status()); };
  return ports;
}

class FakeMediaHost final : public CallMediaHost {
public:
  Roe<std::string> P2pLocalIdentity() const override { return std::string("account:local"); }
  Roe<void> P2pSendDirect(const std::string& /*peer*/, CallControlType /*type*/,
                          const std::string& /*detail*/, const std::string& /*display*/) override {
    return {};
  }
  void P2pNotifyRingChanged() override { ++ring_notifies; }
  void P2pSetLastMediaError(std::string message) override { last_error = std::move(message); }
  Roe<std::optional<std::string>> P2pPeerIdentityForCall(const std::string& /*call_id*/) const override {
    return std::optional<std::string>("account:peer");
  }
  Roe<std::optional<std::string>> RelayIdentityForMeshPeerId(const std::string& /*call_id*/,
                                                             const std::string& /*peer_id*/) const override {
    return std::optional<std::string>("account:peer");
  }
  Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& account) const override {
    if (auto it = account_to_peer.find(account); it != account_to_peer.end()) {
      return std::optional<std::string>{it->second};
    }
    return std::optional<std::string>{};
  }
  bool P2pIsAwaitingSfuRecovery() const override { return false; }
  bool P2pExpectGroupSfuMigration(const std::string& /*call_id*/) const override { return false; }
  void P2pNoteExpectSfuAttach(const std::string& /*call_id*/) override {}
  bool P2pIsSfuAttached() const override { return false; }
  void P2pClearAwaitingSfuRecovery() override {}
  void P2pResendMediaKey(const std::string& /*call_id*/, const std::string& /*peer*/) override {
    ++media_key_resends;
  }
  void P2pRequestInboxSync() override { ++inbox_syncs; }

  std::unordered_map<std::string, std::string> account_to_peer;
  int ring_notifies = 0;
  int media_key_resends = 0;
  int inbox_syncs = 0;
  std::string last_error;
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

  bool IsConnected(const std::string& peer_key) const override {
    return connected.count(peer_key) > 0 && connected.at(peer_key);
  }

  void EnsureAssociation(const std::string& peer_key,
                         std::function<void(Roe<void>)> on_done) override {
    ++ensure_association_calls;
    last_ensure_peer = peer_key;
    if (on_done) {
      if (ensure_result) {
        on_done({});
      } else {
        on_done(Error(ensure_error));
      }
    }
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    const auto it = endpoints.find(peer_key);
    if (it != endpoints.end()) {
      return it->second;
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    ++clear_backoff_calls;
    last_clear_backoff_peer = peer_key;
  }
  void AbortInflightDial(const std::string& peer_key) override {
    ++abort_inflight_calls;
    last_abort_peer = peer_key;
  }
  void DropLink(const std::string& peer_key) override {
    ++drop_link_calls;
    last_drop_link_peer = peer_key;
    connected[peer_key] = false;
  }
  void ClearCallMediaCircuitHop(const std::string& /*peer_key*/) override {}

  bool HasCallMediaCircuitHop(const std::string& peer_key) const override {
    return circuit_hops.count(peer_key) > 0 && circuit_hops.at(peer_key);
  }

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> force_dialable;
  std::unordered_map<std::string, bool> connected;
  std::unordered_map<std::string, bool> circuit_hops;
  int ensure_association_calls = 0;
  int clear_backoff_calls = 0;
  int abort_inflight_calls = 0;
  int drop_link_calls = 0;
  std::string last_ensure_peer;
  std::string last_clear_backoff_peer;
  std::string last_abort_peer;
  std::string last_drop_link_peer;
  bool ensure_result = true;
  std::string ensure_error = "ensure association not available";
};

class FakeCircuitHopReach final : public ICircuitHopReach {
public:
  Roe<void> TryEnsureHopReachable(const std::string& /*hop_peer_id*/) override { return {}; }

  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override {
    ++call_media_ensure_calls;
    last_peer = peer_key;
    if (dial) {
      dial->connected[peer_key] = true;
      dial->circuit_hops[peer_key] = true;
    }
    return call_media_result ? Roe<void>() : Error(call_media_error);
  }

  void TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                        std::function<void(Roe<void>)> on_done,
                                        bool /*allow_circuit*/ = true) override {
    ++call_media_ensure_calls;
    last_peer = peer_key;
    if (dial) {
      dial->connected[peer_key] = true;
      dial->circuit_hops[peer_key] = true;
    }
    if (on_done) {
      on_done(call_media_result ? Roe<void>() : Error(call_media_error));
    }
  }

  FakeDialRegistry* dial = nullptr;
  int call_media_ensure_calls = 0;
  std::string last_peer;
  bool call_media_result = true;
  std::string call_media_error = "circuit hop reach failed";
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
  bool IsActive() const override { return active || half_open; }
  CallMediaDirectConnectParams ActiveParams() const override { return active_params; }
  CallMediaSessionPhase Phase() const override {
    if (active) {
      return CallMediaSessionPhase::MediaReady;
    }
    return half_open ? CallMediaSessionPhase::HelloInbound : CallMediaSessionPhase::Idle;
  }
  CallMediaLinkKind ActiveLinkKind() const override { return link_kind; }
  void Detach() override {
    active = false;
    ++detach_calls;
  }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(Roe<void>)> on_done, int /*timeout_ms*/) override {
    ++connect_async_calls;
    last_params = params;
    if (hang_first_n_connects > 0) {
      --hang_first_n_connects;
      return;
    }
    if (fail_first_n_connects > 0) {
      --fail_first_n_connects;
      if (on_done) {
        on_done(Error(connect_fail_message));
      }
      return;
    }
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
  CallMediaLinkKind link_kind = CallMediaLinkKind::Unknown;
  int connect_async_calls = 0;
  int detach_calls = 0;
  int fail_first_n_connects = 0;
  /** A bundle is mid-handshake (glare / loss): IsActive() true, phase not MediaReady. */
  bool half_open = false;
  int hang_first_n_connects = 0;
  std::string connect_fail_message =
      "amp link: transport failed [adp: adp udp: sendto dst=192.168.0.103:54410 errno=64]";
  CallMediaDirectConnectParams last_params;
  CallMediaDirectConnectParams active_params;
  std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> inbound;
};

class CallMediaBridgeAnswererStartTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize();
    AppRuntime::InitializeUI();
    data_dir_ = std::filesystem::temp_directory_path() / ("pp_bridge_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);
    store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
    ASSERT_TRUE(store_->ListThreads());
    sessions_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
    keys_ = std::make_unique<CallMediaKeyStore>(store_->ProfileDbPath());
    ASSERT_TRUE(keys_->SetDek(TestDek()));
    media_ = std::make_unique<CallMediaEngine>();
    media_->SetSkipDeviceOpenForTest(true);
    host_ = std::make_unique<FakeMediaHost>();
    dial_ = std::make_unique<FakeDialRegistry>();
    circuit_ = std::make_unique<FakeCircuitHopReach>();
    circuit_->dial = dial_.get();
    transport_ = std::make_unique<FakeCallMediaTransport>();
    lifecycle_ = std::make_unique<CallLifecycle>();
    bridge_ = std::make_unique<CallMediaBridge>(*host_, *sessions_, *keys_, *media_, *transport_, dial_.get(),
                                                circuit_.get());
    bridge_->SetDirectArmingPorts(TestDirectArmingPorts(lifecycle_.get()));
    dial_->force_dialable["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWPeer";
  }

  void TearDown() override {
    if (bridge_) {
      bridge_->PrepareForTeardown(0);
    }
    // Join the worker pool before destroying objects a still-running task may touch
    // (see call_session_inbound_compose_test.cpp TearDown).
    AppRuntime::Shutdown();
    bridge_.reset();
    lifecycle_.reset();
    transport_.reset();
    circuit_.reset();
    dial_.reset();
    host_.reset();
    // Always Stop (joins capture) before destroy — headless Windows OpenAudioDevices can
    // still be failing when TearDown runs; budgeted detach previously UAF'd here.
    if (media_) {
      media_->Stop();
    }
    media_.reset();
    if (keys_) {
      keys_->ClearDek();
    }
    keys_.reset();
    sessions_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
    AppRuntime::ShutdownUI();
  }

  void SeedActiveCall(const std::string& call_id) {
    CallSession session;
    session.call_id = call_id;
    session.origin_thread_id = "thread:1";
    session.media_mode = CallMediaMode::Voice;
    session.state = CallSessionState::Active;
    session.created_at = 1;
    session.media_epoch = 1;
    session.media_key_id = "mk:1";
    ASSERT_TRUE(sessions_->UpsertSession(session));
    CallParticipant local;
    local.call_id = call_id;
    local.identity = "account:local";
    local.state = CallParticipantState::Joined;
    local.joined_at = 1;
    ASSERT_TRUE(sessions_->UpsertParticipant(local));
    CallParticipant peer;
    peer.call_id = call_id;
    peer.identity = "account:peer";
    peer.state = CallParticipantState::Joined;
    peer.joined_at = 2;
    ASSERT_TRUE(sessions_->UpsertParticipant(peer));
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<CallSessionStore> sessions_;
  std::unique_ptr<CallMediaKeyStore> keys_;
  std::unique_ptr<CallMediaEngine> media_;
  std::unique_ptr<FakeMediaHost> host_;
  std::unique_ptr<FakeDialRegistry> dial_;
  std::unique_ptr<FakeCircuitHopReach> circuit_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<CallLifecycle> lifecycle_;
  std::unique_ptr<CallMediaBridge> bridge_;
};

// Dogfood 2026-09-24: answerer showed "Punched" while its inbound leg rode a relay carrier.
TEST_F(CallMediaBridgeAnswererStartTest, PathKindFollowsBoundLinkNotReachLoop) {
  transport_->active = true;
  transport_->link_kind = CallMediaLinkKind::Relayed;
  EXPECT_EQ(bridge_->MediaPathKind(), "circuit");

  transport_->link_kind = CallMediaLinkKind::Direct;
  EXPECT_NE(bridge_->MediaPathKind(), "circuit");

  transport_->active = false;
  transport_->link_kind = CallMediaLinkKind::Relayed;
  EXPECT_NE(bridge_->MediaPathKind(), "circuit") << "stale bound kind must not label an idle transport";
}

// k2: relay reservations are a 15 s lease — renew while the media session lives, stop on Stop.
TEST_F(CallMediaBridgeAnswererStartTest, ReservationRenewedWhileSessionLiveStopsOnStop) {
  std::atomic<int> reserves{0};
  bridge_->SetSeedReserve([&] { reserves.fetch_add(1); });
  bridge_->SetReserveRenewIntervalMsForTest(30);

  const std::string call_id = "call:answerer-renew";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();
  ASSERT_GE(reserves.load(), 1) << "BeginSession parks once";

  const auto pump_for = [](std::chrono::milliseconds span) {
    const auto until = std::chrono::steady_clock::now() + span;
    while (std::chrono::steady_clock::now() < until) {
      AppRuntime::RunUITasks();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  // Wait for renewals rather than counting them in a fixed window: coordinator timers are
  // coarse on loaded CI runners (macOS saw the first 30 ms renewal after ~80 ms).
  const auto renew_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (reserves.load() < 3 && std::chrono::steady_clock::now() < renew_deadline) {
    AppRuntime::RunUITasks();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_GE(reserves.load(), 3) << "lease renewed while live";

  bridge_->StopMeshMedia(call_id);
  pump_for(std::chrono::milliseconds(50));
  const int after_stop = reserves.load();
  pump_for(std::chrono::milliseconds(200));
  EXPECT_EQ(reserves.load(), after_stop) << "no renewals after StopMeshMedia";
}

TEST_F(CallMediaBridgeAnswererStartTest, KeyReadyScheduleStartActivatesMedia) {
  // Product glue: ScheduleStartMediaAsAnswerer (Kick / Accept) → BeginSession StartSfu when key present.
  const std::string call_id = "call:answerer-key";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  ASSERT_TRUE(lifecycle_->AllowsDirectPath());

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  EXPECT_TRUE(bridge_->MediaAttempted(call_id));
  EXPECT_TRUE(media_->IsActive());
  EXPECT_EQ(media_->ActiveCallId(), call_id);
  EXPECT_NE(lifecycle_->Phase(), CallPhase::MediaPending);
}

TEST_F(CallMediaBridgeAnswererStartTest, MissingKeyDefersMediaPending) {
  const std::string call_id = "call:answerer-nokey";
  SeedActiveCall(call_id);

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  EXPECT_TRUE(bridge_->MediaAttempted(call_id));
  EXPECT_FALSE(media_->IsActive());
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::MediaPending);
  // Inbox poll is PostWorkerBackground — assert defer contract here; sync may land after RunUITasks.
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, MissingKeyWaitExhaustionConnectFailed) {
  // CALLS / CURRENT_STATE: deferred MediaKey exhaustion → ConnectFailed (not stuck MediaPending).
  const std::string call_id = "call:answerer-key-timeout";
  SeedActiveCall(call_id);

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->SetMediaKeyInboxPollRoundsForTest(0);

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::MediaPending);

  for (int i = 0; i < 500; ++i) {
    AppRuntime::RunUITasks();
    if (lifecycle_->Phase() == CallPhase::ConnectFailed) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::ConnectFailed)
      << "got phase=" << CallPhaseName(lifecycle_->Phase());
  EXPECT_TRUE(bridge_->IsMeshConnectFailed());
  EXPECT_FALSE(host_->last_error.empty());
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, HopLiveStatusDoesNotStartDirectDuplex) {
  const std::string call_id = "call:hop-blocks-direct";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->Apply(CallLifecycleEvent::DirectConnected, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::HopLive, call_id);
  ASSERT_FALSE(lifecycle_->AllowsDirectPath());
  ASSERT_TRUE(lifecycle_->AllowsHopPath());

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  // InCall + HopLive: ScheduleStart must not re-arm Direct or StartSfu.
  EXPECT_FALSE(media_->IsActive());
  EXPECT_EQ(lifecycle_->Status(), CallMediaStatus::HopLive);
}

TEST_F(CallMediaBridgeAnswererStartTest, OffererScheduleStartActivatesMedia) {
  const std::string call_id = "call:offerer-key";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  lifecycle_->Apply(CallLifecycleEvent::OutboundStarted, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  ASSERT_TRUE(lifecycle_->AllowsDirectPath());

  bridge_->ScheduleStartMediaAsOfferer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  EXPECT_TRUE(bridge_->MediaAttempted(call_id));
  EXPECT_TRUE(media_->IsActive());
  EXPECT_EQ(media_->ActiveCallId(), call_id);
  // Offerer Connect waits inbound grace before ConnectAsync — do not assert dial yet.
}

TEST_F(CallMediaBridgeAnswererStartTest, DeferredKeyThenOnMediaKeyReadyActivatesMedia) {
  const std::string call_id = "call:deferred-key";
  SeedActiveCall(call_id);

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();
  EXPECT_EQ(lifecycle_->Phase(), CallPhase::MediaPending);
  EXPECT_FALSE(media_->IsActive());

  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  bridge_->OnMediaKeyReady(call_id);
  AppRuntime::RunUITasks();

  EXPECT_TRUE(media_->IsActive());
  EXPECT_EQ(media_->ActiveCallId(), call_id);
  EXPECT_NE(lifecycle_->Phase(), CallPhase::MediaPending);
}

TEST_F(CallMediaBridgeAnswererStartTest, ReleaseDirectTransportDetachesWithoutStoppingEngine) {
  const std::string call_id = "call:release-direct";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();
  ASSERT_TRUE(media_->IsActive());

  const int detaches_before = transport_->detach_calls;
  bridge_->ReleaseDirectTransport();

  EXPECT_GT(transport_->detach_calls, detaches_before);
  EXPECT_TRUE(media_->IsActive()) << "SoftMigrate ReleaseDirect must keep engine capture";
  EXPECT_EQ(media_->ActiveCallId(), call_id);
}

TEST_F(CallMediaBridgeAnswererStartTest, DialableDialBackoffDoesNotHammerEnsureUsesCircuit) {
  // Dogfood two-net answerer: peer dialable but EnsureAssociation → dial in backoff; circuit
  // still marks Connected. Must not hammer EnsureAssociation every poll; Connect must succeed.
  const std::string call_id = "call:dial-backoff-circuit";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  dial_->ensure_result = false;
  dial_->ensure_error = "amp link: dial in backoff";
  dial_->connected["account:peer"] = false;

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");

  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (transport_->connect_async_calls > 0 || lifecycle_->Phase() == CallPhase::ConnectFailed ||
        lifecycle_->Phase() == CallPhase::InCall) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_GE(circuit_->call_media_ensure_calls, 1) << "circuit reach must run when ADP dial is in backoff";
  EXPECT_LE(dial_->ensure_association_calls, 2)
      << "must not hammer EnsureAssociation while dial in backoff (got "
      << dial_->ensure_association_calls << ")";
  EXPECT_GE(dial_->clear_backoff_calls, 1) << "Ensure miss must ClearDialBackoff for circuit pivot";
  // AbortInflightDial before ConnectAsync is OK; EnsureAssociation *miss callback* must not Abort
  // (dial already finished — double ScheduleDropLink AVs). Miss path only Clears backoff.
  EXPECT_NE(lifecycle_->Phase(), CallPhase::ConnectFailed)
      << "circuit Connected should prevent ConnectFailed; phase="
      << CallPhaseName(lifecycle_->Phase()) << " err=" << host_->last_error;
  EXPECT_GT(transport_->connect_async_calls, 0);
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, CircuitHopMissStopsMediaOnConnectFailed) {
  // Dogfood: Connect give-up must stop StartSfu capture (no zombie TX) and Direct→Idle.
  const std::string call_id = "call:circuit-miss-stop";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  dial_->ensure_result = false;
  dial_->ensure_error = "amp link manager: dial timeout [link: amp link: dial timeout]";
  dial_->connected["account:peer"] = false;
  circuit_->call_media_result = false;
  circuit_->call_media_error = "circuit hop reach failed: relay !endpoint";
  // FakeCircuit must not mark connected on miss.
  circuit_->dial = nullptr;

  bridge_->SetDialWaitBudgetMsForTest(400);
  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();
  ASSERT_TRUE(media_->IsActive()) << "BeginSession starts engine before Ensure settles";

  for (int i = 0; i < 300; ++i) {
    AppRuntime::RunUITasks();
    if (lifecycle_->Phase() == CallPhase::ConnectFailed && !media_->IsActive()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(lifecycle_->Phase(), CallPhase::ConnectFailed)
      << "err=" << host_->last_error;
  EXPECT_TRUE(bridge_->IsMeshConnectFailed());
  EXPECT_FALSE(media_->IsActive()) << "ConnectFailed must StopMeshMedia (no zombie TX)";
  EXPECT_EQ(bridge_->DirectPlannerPhase(), CallDirectPlannerPhase::Idle);
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, EnsureReachResolvesAccountToMeshPeerId) {
  // Hard-lab / dogfood: BeginSession peer is account:; circuit StartBridge needs Amp PeerId.
  const std::string call_id = "call:account-to-peerid";
  const std::string mesh_peer = "12D3KooWEnsurePeerIdTarget";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  host_->account_to_peer["account:peer"] = mesh_peer;
  dial_->endpoints.clear();
  dial_->connected.clear();
  dial_->force_dialable.clear();

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");

  for (int i = 0; i < 200; ++i) {
    AppRuntime::RunUITasks();
    if (circuit_->call_media_ensure_calls > 0 || lifecycle_->Phase() == CallPhase::ConnectFailed ||
        transport_->connect_async_calls > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  ASSERT_GE(circuit_->call_media_ensure_calls, 1);
  EXPECT_EQ(circuit_->last_peer, mesh_peer)
      << "TryEnsureCallMediaReachable must use MeshPeerId, not account:";
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, StaleConnectedLinkIsDroppedAndRedialed) {
  // B39: amp still reports the old link Connected after the peer changed network — the first
  // ConnectAsync hits a dead link ("transport failed"); the bridge must DropLink and redial
  // instead of reusing the same stale link on every attempt.
  const std::string call_id = "call:stale-link";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  dial_->connected["account:peer"] = true;
  // After DropLink resets connected=false, the redial's Ensure should fall back to circuit fast
  // (short budget) rather than loop the "ok but not connected" backoff.
  dial_->ensure_result = false;
  dial_->ensure_error = "amp link: dial in backoff";
  bridge_->SetDialWaitBudgetMsForTest(300);
  transport_->fail_first_n_connects = 1;

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  ASSERT_TRUE(lifecycle_->AllowsDirectPath());

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  for (int i = 0; i < 400; ++i) {
    AppRuntime::RunUITasks();
    if (transport_->connect_async_calls >= 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(dial_->drop_link_calls, 1);
  EXPECT_EQ(dial_->last_drop_link_peer, "account:peer");
  EXPECT_EQ(transport_->connect_async_calls, 2);
  // Answerer + private Preferred skips EnsureAssociation and goes straight to circuit (existing
  // behavior) — the signal that attempt 2 actually redialed (rather than short-circuiting on the
  // stale "connected" link) is that Ensure ran the reachability path again.
  EXPECT_GE(circuit_->call_media_ensure_calls, 1)
      << "second attempt must redial via Ensure, not short-circuit on the stale link";
  EXPECT_TRUE(media_->IsActive());
  EXPECT_EQ(media_->ActiveCallId(), call_id);
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, WatchdogFailsOnlyTheAttempt) {
  // B42: the per-attempt ConnectAsync watchdog must fail only the stuck attempt, not the whole
  // 5-attempt sequence — attempt 2 must still be dialed and allowed to succeed.
  const std::string call_id = "call:watchdog-attempt";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));

  dial_->ensure_result = false;
  dial_->ensure_error = "amp link: dial in backoff";
  dial_->connected["account:peer"] = false;
  bridge_->SetDialWaitBudgetMsForTest(300);
  bridge_->SetConnectAttemptTimeoutMsForTest(200);
  transport_->hang_first_n_connects = 1;

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  ASSERT_TRUE(lifecycle_->AllowsDirectPath());

  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  // Watchdog margin (timeout + 1000ms) + 1500ms retry — allow generous wall-clock slack.
  for (int i = 0; i < 500; ++i) {
    AppRuntime::RunUITasks();
    if (transport_->connect_async_calls >= 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(transport_->connect_async_calls, 2)
      << "watchdog must have failed only attempt 1; attempt 2 must still be dialed";
  EXPECT_TRUE(media_->IsActive());
  EXPECT_EQ(media_->ActiveCallId(), call_id);
  bridge_->PrepareForTeardown(0);
}

TEST_F(CallMediaBridgeAnswererStartTest, HalfOpenBundleIsNotAConnection) {
  // Hard lab CGNAT stack, delay 80 ms + 1 % loss: a failed attempt while a glare bundle sat in
  // hello was committed Live ("InCall", no media, no retry). Only MediaReady is connected.
  const std::string call_id = "call:half-open";
  SeedActiveCall(call_id);
  ASSERT_TRUE(keys_->PutEpochKey(call_id, 1, TestMediaKey()));
  dial_->ensure_result = false;
  dial_->ensure_error = "amp link: dial in backoff";
  dial_->connected["account:peer"] = false;
  bridge_->SetDialWaitBudgetMsForTest(300);
  bridge_->SetConnectAttemptTimeoutMsForTest(200);
  transport_->half_open = true;
  transport_->fail_first_n_connects = 1;

  lifecycle_->Apply(CallLifecycleEvent::AcceptSucceeded, call_id);
  lifecycle_->SetMediaStatus(CallMediaStatus::DirectConnecting, call_id);
  bridge_->ScheduleStartMediaAsAnswerer(call_id, "account:peer");
  AppRuntime::RunUITasks();

  for (int i = 0; i < 500 && transport_->connect_async_calls < 2; ++i) {
    AppRuntime::RunUITasks();
    if (transport_->connect_async_calls < 2) {  // attempt 2 may legitimately connect
      EXPECT_NE(lifecycle_->Status(), CallMediaStatus::DirectLive) << "half-open bundle committed as Live";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(transport_->connect_async_calls, 2) << "failed attempt must be retried, not taken as connected";
  bridge_->PrepareForTeardown(0);
}

} // namespace
} // namespace pbr
