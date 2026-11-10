#pragma once

#include "lib/calls/testing/InMemoryCallStores.h"
#include "common/chat/SystemControl.h"

// Shared fixture pieces for multi-CallStack compose tests (dual-stack 1:1, group SFU): one full
// CallStack + CallUiBackend per participant over its own profile stores, with call-control
// delivered by the test and the 1:1 transport / dial registry faked.

#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"

#include "domain/call_media/ICallMediaTransport.h"
#include "lib/calls/CallTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/MeshConfig.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/Utilities.h"
#include "common/thread/ThreadRecordTypes.h"
#include "feature/calls/tests/call_media_inbound_fake.h"
#include "feature/calls/tests/seeded_session_key.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
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
  void Detach(const std::string& /*call_id*/) override {
    active = false;
    ++detach_calls;
  }
  void DetachAll() override { Detach({}); }
  /** K014: a hang-up says bye (counted), then detaches. */
  void HangUp(const std::string& call_id) override {
    ++hangup_calls;
    Detach(call_id);
  }
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(CallMediaRoe)> on_done, int /*timeout_ms*/) override {
    ++connect_async_calls;
    last_params = params;
    last_callbacks = callbacks;
    if (fail_connects) {
      if (on_done) {
        on_done(CallMediaFailure::Of(CallMediaErr::Generic, "fake call-media connect failed"));
      }
      return;
    }
    active = true;
    active_params = params;
    if (peer_inbound) {
      // Simulate reverse-dial landing on the peer's CallDirectPath inbound handler.
      peer_inbound(params);
    }
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

  bool started = false;
  bool active = false;
  /** Every outbound connect fails (no link to the peer); inbound hellos still land. */
  bool fail_connects = false;
  int connect_async_calls = 0;
  int detach_calls = 0;
  int hangup_calls = 0;
  /** The last connect's callbacks: a test plays the transport's events through them. */
  CallMediaDirectCallbacks last_callbacks;
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
  /** The stack binds its call-control family here and unbinds it on teardown: declared first. */
  SystemControlRegistry controls;
  std::filesystem::path data_dir;
  std::unique_ptr<InMemoryDeliveryStore> store;
  std::unique_ptr<ContactsStore> contacts;
  std::unique_ptr<IdentityStore> identity;
  IPeerSessionStore* psk = nullptr; // the store's peer sessions (one row with its threads, as in the app)
  // Outlive the stack (connectivity first: the relay is built on it).
  std::unique_ptr<mesh::MeshConnectivity> connectivity = std::make_unique<mesh::MeshConnectivity>();
  std::unique_ptr<mesh::MeshMediaRelay> media_relay = std::make_unique<mesh::MeshMediaRelay>(*connectivity);
  std::unique_ptr<CallStack> stack;
  std::unique_ptr<CallUiBackend> ui;
  std::unique_ptr<FakeCallMediaTransport> transport;
  std::unique_ptr<FakeDialRegistry> dial;
  MeshConfig mesh_config;
  std::string local_identity;
  /** Call-control this side sends (its `send_user_message`); the test delivers it. */
  std::function<void(const std::string& peer, const ThreadMessage& msg)> on_send;
  /** Call summaries this side wrote into the chat stream (its send_to_thread). */
  std::vector<ThreadMessage> summaries;
  /** What the stack was built with (a test may build its sessions again). */
  CallStackDeps deps;

  /** Deliver call control to this side, as the receive path does: to the family that owns it. */
  Roe<void> Deliver(ThreadMessage msg, const std::string& sender) const {
    const auto family = controls.Find(msg);
    if (!family || !family->apply) {
      return Error("no control family bound for the message");
    }
    return family->apply(msg, SystemControlContext{sender, std::nullopt, std::nullopt});
  }
};

struct StackSideOptions {
  /** Hub `seed_dial_ok`: org seeds (mesh bootstrap peers) count as media_relay hop candidates. */
  bool seed_dial_ok = false;
  /** Stands in for the hub's media_relay client (group calls); null = none. */
  mesh::IMediaRelayClient* relay = nullptr;
  /** The hub's group roster as the calls see it (who is in a group now); unset = not checked. */
  std::function<std::optional<std::set<std::string>>(const std::string& group_id)> group_members;
};

/** Build `side` (tag = profile id, dirs under temp). Set `side.mesh_config` / `on_send` before. */
inline void BuildStackSide(StackSide& side, const std::string& tag, uint8_t dek_seed,
                           const StackSideOptions& options = {}) {
  side.data_dir = std::filesystem::temp_directory_path() / ("pp_stack_" + tag + "_" + util::GenerateUuid());
  std::filesystem::remove_all(side.data_dir);
  std::filesystem::create_directories(side.data_dir);

  side.store = std::make_unique<InMemoryDeliveryStore>();
  ASSERT_TRUE(side.store->ListThreads());
  side.contacts = std::make_unique<ContactsStore>(side.data_dir.string());
  side.identity = std::make_unique<IdentityStore>(side.data_dir.string(), tag);
  ASSERT_TRUE(side.identity->SetDek(TestDek(dek_seed)));
  auto loaded = side.identity->LoadOrCreate();
  ASSERT_TRUE(loaded) << loaded.error().message;
  side.local_identity = loaded->account_id;
  ASSERT_FALSE(side.local_identity.empty());

  side.psk = side.store.get();

  side.stack = std::make_unique<CallStack>();
  ASSERT_TRUE(side.stack->InitializeStores(InMemoryCallStores()));
  side.ui = std::make_unique<CallUiBackend>(*side.stack);

  CallStackDeps deps;
  deps.threads = side.store.get();
  deps.transcript = side.store.get();
  deps.contacts = side.contacts.get();
  deps.account = side.identity.get();
  deps.delivery.ensure_peer_session_key = [&side](const std::string& peer) {
    return SeededSessionKey(*side.psk, peer);
  };
  deps.delivery.held_peer_session_key = deps.delivery.ensure_peer_session_key;
  deps.delivery.send_control = [&side](const std::string& peer, const ThreadMessage& control,
                                       bool /*urgent*/) -> Roe<void> {
    if (side.on_send) {
      side.on_send(peer, control);
    }
    return {};
  };
  deps.delivery.send_control_to_many = ControlSendsOneByOne(deps.delivery.send_control);
  deps.delivery.group_members = options.group_members;
  deps.delivery.send_to_thread = [&side](const std::string& thread_id, const std::string& text,
                                         const SendRelayOptions& send_options) -> Roe<ThreadMessage> {
    ThreadMessage msg;
    msg.id = util::GenerateUuid();
    msg.thread_id = thread_id;
    msg.text = text;
    msg.sender_contact_id = kLocalSelfContactId;
    msg.content_type = send_options.content_type.value_or(ChatContentType::System);
    msg.payload_json = send_options.payload_json.value_or("");
    side.summaries.push_back(msg);
    return msg;
  };
  deps.delivery.resolve_peer_thread = [](const std::string& peer) -> Roe<std::string> { return "dm:" + peer; };
  deps.delivery.resolve_group_thread = [&side](const std::string& group_id) -> std::optional<std::string> {
    if (auto threads = side.store->ListThreads()) {
      for (const Thread& thread : *threads) {
        if (thread.kind == ThreadKind::Group && thread.group_id == group_id) {
          return thread.id;
        }
      }
    }
    return std::nullopt;
  };
  deps.delivery.sync_inbox_from_wake = [](bool) {};
  deps.mesh_config = [&side]() { return std::make_shared<const MeshConfig>(side.mesh_config); };
  deps.mesh = []() -> mesh::MeshHost* { return nullptr; };
  deps.list_directory_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
  deps.list_dht_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
  deps.seed_dial_ok = [ok = options.seed_dial_ok]() { return ok; };
  deps.prefetch_peer_reachability = [](const std::string&) {};
  deps.sync_mobile_ephemeral_listen = []() {};
  deps.system_controls = &side.controls;

  deps.connectivity = side.connectivity.get();
  deps.media_relay = side.media_relay.get();
  side.deps = deps;
  side.stack->BuildSessions(deps);
  ASSERT_TRUE(side.ui->Available());
  ASSERT_FALSE(side.controls.Snapshot()->empty()) << "the call family is bound";
  if (MediaEngine* media = side.stack->Engine()) {
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
  side.psk = nullptr;
  side.identity.reset();
  side.contacts.reset();
  side.store.reset();
  if (!side.data_dir.empty()) {
    std::filesystem::remove_all(side.data_dir);
    side.data_dir.clear();
  }
}

} // namespace pbr::test
