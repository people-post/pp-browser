#pragma once

// Shared fixture pieces for multi-CallStack compose tests (dual-stack 1:1, group SFU): one full
// CallStack + CallUiBackend per participant over its own profile stores, with call-control
// delivered by the test and the 1:1 transport / dial registry faked.

#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"

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
#include "feature/conversations/tests/call_media_inbound_fake.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pbr::test {

inline ByteVector TestDek(uint8_t seed) {
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
  void ClearPeerCircuitHop(const std::string& /*peer_key*/) override {}

  std::unordered_map<std::string, std::string> endpoints;
  std::unordered_map<std::string, bool> connected;
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
    if (fail_connects) {
      if (on_done) {
        on_done(Error("fake call-media connect failed"));
      }
      return;
    }
    active = true;
    active_params = params;
    if (peer_inbound) {
      // Simulate reverse-dial landing on the peer's CallMediaBridge inbound handler.
      peer_inbound(params);
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
  /** Every outbound connect fails (no link to the peer); inbound hellos still land. */
  bool fail_connects = false;
  int connect_async_calls = 0;
  int detach_calls = 0;
  CallMediaDirectConnectParams last_params;
  CallMediaDirectConnectParams active_params;
  InboundHelloFake inbound;
  /** Reverse dial: deliver a hello to the peer; its answer connects the peer's side. */
  std::function<void(CallMediaDirectConnectParams)> peer_inbound;
};

inline void DrainUntil(const std::function<bool()>& done, int max_ms = 6000) {
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

/** CallUiBackend::StartCall is an intent: run the calls owner until it reports. */
inline Roe<CallSession> StartCallNow(CallUiBackend& ui, const std::string& thread_id, bool video,
                                     const std::vector<std::string>& invitees) {
  std::optional<Roe<CallSession>> started;
  ui.StartCall(thread_id, video, invitees, [&started](Roe<CallSession> result) { started = std::move(result); });
  for (int i = 0; i < 1000 && !started; ++i) {
    AppRuntime::RunUIAndOwnerTasks();
  }
  return started ? *started : Roe<CallSession>(Error("StartCall did not report"));
}

/** One participant: profile stores + CallStack + CallUiBackend + the faked 1:1 media path. */
struct StackSide {
  std::filesystem::path data_dir;
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<ContactsStore> contacts;
  std::unique_ptr<IdentityStore> identity;
  std::unique_ptr<SqlitePskSessionStore> psk;
  std::unique_ptr<MeshMediaPlane> mesh_media = std::make_unique<MeshMediaPlane>();  // outlives stack
  std::unique_ptr<CallStack> stack;
  std::unique_ptr<CallUiBackend> ui;
  std::unique_ptr<FakeCallMediaTransport> transport;
  std::unique_ptr<FakeDialRegistry> dial;
  CallControlInboundPorts inbound;
  AppConfig app_config;
  std::string local_identity;
  /** Call-control this side sends (its `send_user_message`); the test delivers it. */
  std::function<void(const ThreadMessage& msg)> on_send;
};

struct StackSideOptions {
  /** Hub `seed_dial_ok`: org seeds (mesh bootstrap peers) count as media_relay hop candidates. */
  bool seed_dial_ok = false;
  /** Stands in for the hub's media_relay client (group calls); null = none. */
  IMediaRelayClient* relay = nullptr;
};

/** Build `side` (tag = profile id, dirs under temp). Set `side.app_config` / `on_send` before. */
inline void BuildStackSide(StackSide& side, const std::string& tag, uint8_t dek_seed,
                           const StackSideOptions& options = {}) {
  side.data_dir = std::filesystem::temp_directory_path() / ("pp_stack_" + tag + "_" + util::GenerateUuid());
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
                                            const SendRelayOptions& send_options) -> Roe<ThreadMessage> {
    ThreadMessage msg;
    msg.id = util::GenerateUuid();
    msg.thread_id = thread_id;
    msg.text = text;
    msg.content_type = send_options.content_type.value_or(ChatContentType::System);
    msg.payload_json = send_options.payload_json.value_or("");
    msg.timestamp = util::NowUnixMs();
    if (side.on_send) {
      side.on_send(msg);
    }
    return msg;
  };
  deps.delivery.sync_inbox_from_wake = [](bool) {};
  deps.mesh_config = [&side]() { return std::make_shared<const MeshConfig>(side.app_config.mesh); };
  deps.mesh = []() -> MeshHost* { return nullptr; };
  deps.list_directory_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
  deps.list_dht_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
  deps.seed_dial_ok = [ok = options.seed_dial_ok]() { return ok; };
  deps.prefetch_peer_reachability = [](const std::string&) {};
  deps.sync_mobile_ephemeral_listen = []() {};
  deps.bind_call_control = [&side](CallControlInboundPorts ports) { side.inbound = std::move(ports); };

  deps.mesh_media = side.mesh_media.get();
  side.stack->BuildSessions(deps);
  ASSERT_TRUE(side.ui->Available());
  ASSERT_TRUE(side.inbound.apply_inbound_control);
  if (CallMediaEngine* media = side.stack->MediaEngine()) {
    media->SetSkipDeviceOpenForTest(true);
  }

  side.transport = std::make_unique<FakeCallMediaTransport>();
  side.dial = std::make_unique<FakeDialRegistry>();
  side.stack->BindTestMediaPath(side.transport.get(), side.dial.get(), nullptr, options.relay);
}

/** Stop the stack (before AppRuntime shutdown). */
inline void SoftStopStackSide(StackSide& side) {
  side.ui.reset();
  if (side.stack) {
    side.stack->AbortCallMediaForShutdown();
    side.stack->Shutdown();
  }
  side.stack.reset();
  side.transport.reset();
  side.dial.reset();
}

/** Drop the stores and the profile dir (after AppRuntime shutdown joined the pool). */
inline void DestroyStackSide(StackSide& side) {
  if (side.psk) {
    side.psk->ClearDek();
  }
  side.psk.reset();
  side.identity.reset();
  side.contacts.reset();
  side.store.reset();
  if (!side.data_dir.empty()) {
    std::filesystem::remove_all(side.data_dir);
    side.data_dir.clear();
  }
}

} // namespace pbr::test
