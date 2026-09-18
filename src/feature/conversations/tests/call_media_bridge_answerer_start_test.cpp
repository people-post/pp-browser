#include "feature/calls/CallMediaBridge.h"
#include "feature/calls/CallLifecycle.h"
#include "feature/calls/CallDirectArmingPorts.h"
#include "feature/calls/CallTopologyRelayDeps.h"

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
  Roe<std::optional<std::string>> MeshPeerIdForAccount(const std::string& /*account*/) const override {
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
    host_ = std::make_unique<FakeMediaHost>();
    dial_ = std::make_unique<FakeDialRegistry>();
    transport_ = std::make_unique<FakeCallMediaTransport>();
    lifecycle_ = std::make_unique<CallLifecycle>();
    bridge_ = std::make_unique<CallMediaBridge>(*host_, *sessions_, *keys_, *media_, *transport_, dial_.get(),
                                                nullptr);
    bridge_->SetDirectArmingPorts(MakeCallDirectArmingPorts(lifecycle_.get()));
    dial_->force_dialable["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/10.0.0.2/udp/1/p2p/12D3KooWPeer";
  }

  void TearDown() override {
    if (bridge_) {
      bridge_->PrepareForTeardown(0);
    }
    bridge_.reset();
    lifecycle_.reset();
    transport_.reset();
    dial_.reset();
    host_.reset();
    if (media_ && media_->IsActive()) {
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
    AppRuntime::Shutdown();
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
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<CallLifecycle> lifecycle_;
  std::unique_ptr<CallMediaBridge> bridge_;
};

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

} // namespace
} // namespace pbr
