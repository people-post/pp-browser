#include "lib/calls/testing/InMemoryCallStores.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/calls/CallHopRelayDeps.h"

#include "domain/call_media/ICallMediaTransport.h"
#include "lib/calls/CallControlCodec.h"
#include "lib/calls/CallTypes.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/MeshConfig.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "common/media/MediaSessionsOwner.h"
#include "common/Utilities.h"
#include "common/thread/ThreadRecordTypes.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include "feature/calls/tests/call_media_inbound_fake.h"
#include "common/MeshAlias.h"

#include <gtest/gtest.h>
#include <optional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pbr {
namespace {

/** Any call-control message: the call family owns it once bound. */
ThreadMessage CallControlProbeMessage() {
  return CallControlCodec::BuildSystemMessage("thread:probe", CallControlType::CallEnded, "Call ended", "{}",
                                              "account:peer")
      .value();
}

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
  void ConnectAsync(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    std::function<void(CallMediaRoe)> on_done, int /*timeout_ms*/) override {
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
  int connect_async_calls = 0;
  int detach_calls = 0;
  CallMediaDirectConnectParams last_params;
  CallMediaDirectConnectParams active_params;
  test::InboundHelloFake inbound;
};

void DrainUntil(const std::function<bool()>& done, int max_ms = 4000) {
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
Roe<CallSession> StartCallNow(CallUiBackend& ui, const std::string& thread_id, bool video,
                              const std::vector<std::string>& invitees) {
  std::optional<Roe<CallSession>> started;
  ui.StartCall(thread_id, video, invitees, [&started](Roe<CallSession> result) { started = std::move(result); });
  for (int i = 0; i < 1000 && !started; ++i) {
    AppRuntime::RunUIAndOwnerTasks();
  }
  return started ? *started : Roe<CallSession>(Error("StartCall did not report"));
}

class CallUiBackendStackTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    AppRuntime::Initialize(MediaSessionsManualRuntimeConfig());
    AppRuntime::InitializeUI();

    data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_stack_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    std::filesystem::create_directories(data_dir_);

    store_ = std::make_unique<InMemoryDeliveryStore>();
    ASSERT_TRUE(store_->ListThreads());
    contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
    identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "stack-test");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    auto loaded = identity_->LoadOrCreate();
    ASSERT_TRUE(loaded) << loaded.error().message;
    local_identity_ = loaded->account_id;

    mesh_config_ = MeshConfig{};
    listen_desires_ = 0;

    stack_ = std::make_unique<CallStack>();
    ASSERT_TRUE(stack_->InitializeStores(InMemoryCallStores()));

    ui_ = std::make_unique<CallUiBackend>(*stack_);
    EXPECT_FALSE(ui_->Available()) << "before BuildSessions";

    CallStackDeps deps;
    deps.threads = store_.get();
    deps.transcript = store_.get();
    deps.contacts = contacts_.get();
    deps.account = identity_.get();
    deps.delivery.send_control = [this](const std::string& /*peer*/, const ThreadMessage& control,
                                        bool /*urgent*/) -> Roe<void> {
      ++sent_control_;
      {
        std::lock_guard lock(sent_mu_);
        sent_payloads_.push_back(control.payload_json);
      }
      return {};
    };
    deps.delivery.send_control_to_many = ControlSendsOneByOne(deps.delivery.send_control);
    deps.delivery.send_to_thread = [this](const std::string& thread_id, const std::string& text,
                                          const SendRelayOptions& options) -> Roe<ThreadMessage> {
      ThreadMessage msg;
      msg.id = util::GenerateUuid();
      msg.thread_id = thread_id;
      msg.text = text;
      msg.sender_contact_id = kLocalSelfContactId;
      msg.content_type = options.content_type.value_or(ChatContentType::System);
      msg.payload_json = options.payload_json.value_or("");
      std::lock_guard lock(sent_mu_);
      summaries_.push_back(msg);
      return msg;
    };
    deps.delivery.resolve_peer_thread = [](const std::string& peer) -> Roe<std::string> { return "dm:" + peer; };
    deps.delivery.sync_inbox_from_wake = [this](bool /*force*/) { ++inbox_syncs_; };
    deps.mesh_config = [this]() { return std::make_shared<const MeshConfig>(mesh_config_); };
    deps.mesh = []() -> mesh::MeshHost* { return nullptr; };
    deps.list_directory_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
    deps.list_dht_nodes = []() { return std::vector<mesh::MeshDirectoryNode>{}; };
    deps.seed_dial_ok = []() { return false; };
    deps.prefetch_peer_reachability = [](const std::string&) {};
    deps.sync_mobile_ephemeral_listen = [this]() { ++listen_desires_; };
    deps.system_controls = &controls_;

    deps.connectivity = &connectivity_;
    deps.media_relay = &media_relay_;
    mesh::AddressDisclosurePolicy everyone;
    everyone.audience = mesh::DirectAudience::Everyone;
    disclosure_.Publish(everyone);
    deps.address_disclosure = &disclosure_;
    deps_ = deps;
    stack_->BuildSessions(deps);
    ASSERT_TRUE(ui_->Available());
    ASSERT_TRUE(controls_.Find(CallControlProbeMessage())) << "the call family is bound";
    sessions_identity_ = ui_->SessionsIdentity();
    if (MediaEngine* media = stack_->Engine()) {
      media->SetSkipDeviceOpenForTest(true);
    }

    transport_ = std::make_unique<FakeCallMediaTransport>();
    dial_ = std::make_unique<FakeDialRegistry>();
    dial_->connected["account:peer"] = true;
    dial_->endpoints["account:peer"] = "/ip4/127.0.0.1/udp/47100/adp/1.0.0/p2p/12D3KooWPeer";
    stack_->BindTestMediaPath(transport_.get(), dial_.get());
  }

  void TearDown() override {
    // Finish LeaveCall/DeclineInvite workers before destroying the stack.
    (void)AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000));
    ui_.reset();
    if (stack_) {
      stack_->AbortCallMediaForShutdown();
      stack_->Shutdown();
    }
    stack_.reset();
    transport_.reset();
    dial_.reset();
    AppRuntime::ShutdownUI();
    // Join the pool before resetting stores a worker may still touch (PR #216 follow-up).
    AppRuntime::Shutdown();
    identity_.reset();
    contacts_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
  }

  Roe<void> IngestInvite(const std::string& call_id, std::optional<CallPeerCaps> caps = std::nullopt) {
    CallInviteDetail invite;
    if (caps) {
      invite.caps = *caps;
    }
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_mode = CallMediaMode::Voice;
    invite.video_allowed = false;
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    return IngestInviteDetail(invite);
  }

  Roe<void> IngestInviteDetail(const CallInviteDetail& invite, const std::string& sender = "account:peer") {
    auto detail = CallControlCodec::EncodeInvite(invite);
    if (!detail) {
      return detail.error();
    }
    auto msg = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallInvite, "Incoming call",
                                                    *detail, sender);
    if (!msg) {
      return msg.error();
    }
    const auto family = controls_.Find(*msg);
    if (!family || !family->apply) {
      return Error("inbound not bound");
    }
    auto applied = family->apply(*msg, SystemControlContext{sender, std::nullopt, std::nullopt});
    AppRuntime::RunUIAndOwnerTasks(); // call control is applied on the calls owner (drained by the test)
    return applied;
  }

  // First: the call stack unbinds its family here on teardown.
  SystemControlRegistry controls_;
  std::filesystem::path data_dir_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  // Outlive stack_ (declared first; connectivity before the relay built on it).
  mesh::MeshConnectivity connectivity_;
  mesh::MeshMediaRelay media_relay_{connectivity_};
  mesh::AddressDisclosureGate disclosure_;
  std::unique_ptr<CallStack> stack_;
  std::unique_ptr<CallUiBackend> ui_;
  std::unique_ptr<FakeCallMediaTransport> transport_;
  std::unique_ptr<FakeDialRegistry> dial_;
  MeshConfig mesh_config_;
  const void* sessions_identity_ = nullptr;
  std::string local_identity_;
  int sent_control_ = 0;
  std::mutex sent_mu_;
  std::vector<std::string> sent_payloads_;
  std::vector<ThreadMessage> summaries_;
  int inbox_syncs_ = 0;
  int listen_desires_ = 0;
  /** Ring-changed may fire on IO/worker threads (LeaveCall runs on Critical) — fixture-owned, atomic. */
  std::atomic<int> ring_changes_{0};
  // Fixture-owned: chrome refreshes posted after the test body still fire during TearDown's drain.
  std::atomic<int> chrome_refreshes_{0};
  CallStackDeps deps_;
  /** Invites "who can call me" dropped, as the GUI hears them (on UI). */
  std::vector<BlockedCallInvite> blocked_;

  void ListenForBlocked() {
    ui_->SetOnInviteBlocked([this](const BlockedCallInvite& blocked) {
      EXPECT_TRUE(AppRuntime::CurrentlyOnUI());
      blocked_.push_back(blocked);
    });
    AppRuntime::RunUIAndOwnerTasks();
  }

  CallInviteDetail StrangerInvite(const std::string& call_id) {
    CallInviteDetail invite;
    invite.call_id = call_id;
    invite.inviter_identity = "account:peer";
    invite.invitee_identity = local_identity_;
    invite.media_epoch = 1;
    invite.media_key_id = "mk:1";
    invite.expires_at = util::NowUnixMs() + kDefaultCallInviteTtlMs;
    return invite;
  }
};

TEST_F(CallUiBackendStackTest, AvailableAndSessionsIdentityStable) {
  EXPECT_TRUE(ui_->Available());
  EXPECT_EQ(ui_->SessionsIdentity(), sessions_identity_);
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  EXPECT_FALSE(stack_->HasActiveLocalCall());
  EXPECT_FALSE(stack_->WantEphemeralListen());
}

// projects/privacy T3: "who can call me" — with contacts only, a stranger's invite never rings (and
// nothing is sent back); a contact's does.
TEST_F(CallUiBackendStackTest, ContactsOnlyCallAudienceDropsAStrangersInvite) {
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  AppRuntime::RunUIAndOwnerTasks();
  const int sent_before = sent_control_;
  ASSERT_TRUE(IngestInvite("call:stranger"));
  ui_->Apply(CallLifecycleEvent::InviteSeen, "call:stranger");
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle) << "a stranger's call rang";
  EXPECT_EQ(sent_control_, sent_before) << "the drop answered the caller";

  mesh::AddressDisclosurePolicy with_peer;
  with_peer.audience = mesh::DirectAudience::Everyone;
  with_peer.contacts.insert("account:peer");
  disclosure_.Publish(with_peer);
  ASSERT_TRUE(IngestInvite("call:contact"));
  ui_->Apply(CallLifecycleEvent::InviteSeen, "call:contact");
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(ui_->Phase(), CallPhase::Ringing);
  EXPECT_EQ(ui_->LastRingCallId(), "call:contact");
}

// DEV/PLAN-call-stranger-declined.md (a): the dropped invite is told to this device's UI — once, with its call id
// and the setting — and still nothing goes back to the caller.
TEST_F(CallUiBackendStackTest, AnInviteTheAudienceDropsIsToldToTheUiOnce) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  const int sent_before = sent_control_;
  ASSERT_TRUE(IngestInvite("call:stranger"));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_EQ(blocked_[0].call_id, "call:stranger");
  EXPECT_EQ(blocked_[0].sender_identity, "account:peer");
  EXPECT_EQ(blocked_[0].audience, mesh::InboundAudience::ContactsOnly);
  EXPECT_FALSE(blocked_[0].from_group);
  EXPECT_FALSE(blocked_[0].expired);
  EXPECT_EQ(sent_control_, sent_before) << "the caller was told";
}

TEST_F(CallUiBackendStackTest, NobodyDropsAContactsInviteAndSaysSo) {
  ListenForBlocked();
  mesh::AddressDisclosurePolicy with_peer;
  with_peer.audience = mesh::DirectAudience::Everyone;
  with_peer.contacts.insert("account:peer");
  disclosure_.Publish(with_peer);
  stack_->SetInboundCallAudience(mesh::InboundAudience::Nobody);
  ASSERT_TRUE(IngestInvite("call:contact"));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_EQ(blocked_[0].call_id, "call:contact");
  EXPECT_EQ(blocked_[0].audience, mesh::InboundAudience::Nobody);
}

TEST_F(CallUiBackendStackTest, EveryoneDropsNothingAndTellsNothing) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::Everyone);
  ASSERT_TRUE(IngestInvite("call:anyone"));
  ui_->Apply(CallLifecycleEvent::InviteSeen, "call:anyone");
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_TRUE(blocked_.empty());
  EXPECT_EQ(ui_->Phase(), CallPhase::Ringing);
}

TEST_F(CallUiBackendStackTest, OtherCallControlDroppedFromAStrangerIsNotABlockedCall) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  auto ended = CallControlCodec::BuildSystemMessage("thread:inbox", CallControlType::CallEnded, "Call ended",
                                                    R"({"call_id":"call:nobody-has"})", "account:peer");
  ASSERT_TRUE(ended);
  const auto family = controls_.Find(*ended);
  ASSERT_TRUE(family && family->apply);
  ASSERT_TRUE(family->apply(*ended, SystemControlContext{"account:peer", std::nullopt, std::nullopt}));
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_TRUE(blocked_.empty());
}

TEST_F(CallUiBackendStackTest, AnInviteFromOurOwnAccountIsNeverToldAsBlocked) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::Nobody);
  CallInviteDetail invite = StrangerInvite("call:sibling");
  invite.inviter_identity = local_identity_;
  ASSERT_TRUE(IngestInviteDetail(invite, local_identity_));
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_TRUE(blocked_.empty());
}

TEST_F(CallUiBackendStackTest, ADroppedGroupCallInviteSaysItIsAGroups) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  CallInviteDetail invite = StrangerInvite("call:group");
  invite.origin_group_id = "group:g1";
  ASSERT_TRUE(IngestInviteDetail(invite));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_TRUE(blocked_[0].from_group);
}

TEST_F(CallUiBackendStackTest, ADroppedInviteTooOldToRingIsMarkedExpired) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  CallInviteDetail invite = StrangerInvite("call:old");
  invite.expires_at = util::NowUnixMs() - kCallInviteWireSkewSlackMs - 60'000; // the workflow would not ring it
  ASSERT_TRUE(IngestInviteDetail(invite));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_TRUE(blocked_[0].expired);
}

// A cold start from the call's alert: the invite may be fetched and dropped before the GUI listens.
TEST_F(CallUiBackendStackTest, AnInviteDroppedBeforeAnyoneListensIsToldOnceWhenTheyDo) {
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  ASSERT_TRUE(IngestInvite("call:early"));
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_TRUE(blocked_.empty());
  ListenForBlocked();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_EQ(blocked_[0].call_id, "call:early");
  ListenForBlocked();
  EXPECT_EQ(blocked_.size(), 1u) << "told twice";
}

TEST_F(CallUiBackendStackTest, ARebuiltSessionManagerStillTellsTheListener) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  stack_->ResetSessions();
  stack_->BuildSessions(deps_);
  ASSERT_TRUE(ui_->Available());
  ASSERT_NE(ui_->SessionsIdentity(), nullptr);
  ASSERT_TRUE(IngestInvite("call:after-rebuild"));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(blocked_.size(), 1u);
  EXPECT_EQ(blocked_[0].call_id, "call:after-rebuild");
}

TEST_F(CallUiBackendStackTest, AnInviteFromOurOwnAccountIsNotReported) {
  ListenForBlocked();
  stack_->SetInboundCallAudience(mesh::InboundAudience::ContactsOnly);
  CallInviteDetail invite = StrangerInvite("call:own");
  invite.inviter_identity = local_identity_;
  ASSERT_TRUE(IngestInviteDetail(invite, local_identity_));
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_TRUE(blocked_.empty());
}

TEST_F(CallUiBackendStackTest, InviteAcceptLeaveThroughBackend) {
  const std::string call_id = "call:ui-compose";
  ASSERT_TRUE(IngestInvite(call_id));
  ASSERT_TRUE(stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()));

  ui_->SetOnChromeRefresh([this]() { chrome_refreshes_.fetch_add(1); });
  ui_->SetOnRingChanged([this]() { ring_changes_.fetch_add(1); });

  // Intents run on the calls owner; the GUI reads the snapshot it publishes after each step.
  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(ui_->Phase(), CallPhase::Ringing);
  EXPECT_EQ(ui_->LastRingCallId(), call_id);
  EXPECT_TRUE(stack_->WantEphemeralListen());
  EXPECT_GE(chrome_refreshes_.load(), 1);
  EXPECT_GE(listen_desires_, 1);

  auto pending = ui_->TopPendingInvite();
  ASSERT_TRUE(pending && pending->has_value());
  EXPECT_EQ((*pending)->call_id, call_id);

  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  EXPECT_EQ(ui_->Phase(), CallPhase::Ringing)
      << "posted, not applied: the snapshot changes only after the owner's step";

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
  EXPECT_EQ(controls_.LiveFamily(), std::optional<std::string>("call"));

  auto peer = ui_->PeerIdentityForCall(call_id);
  ASSERT_TRUE(peer && peer->has_value());
  EXPECT_EQ(**peer, "account:peer");

  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value();
  });
  // Leave runs on the calls owner — flush before the body's locals go away.
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
  EXPECT_FALSE(stack_->HasActiveLocalCall());
  EXPECT_GE(ring_changes_.load(), 1);
}

// k6 exit: a pinned class (config) rides this end's accept caps, and this end's policy for the call
// combines it with the caps the invite carried.
TEST_F(CallUiBackendStackTest, PinnedMobilityRidesTheAcceptAndSetsThePolicy) {
  mesh_config_.mobility = "mobile";
  stack_->ReloadMobilityOverride();
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(stack_->LocalMobility(), MobilityClass::Mobile);

  const std::string call_id = "call:mobility";
  CallPeerCaps remote;
  remote.present = true;
  remote.mobility = MobilityClass::Stationary;
  ASSERT_TRUE(IngestInvite(call_id, remote));
  ASSERT_TRUE(stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()));
  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  DrainUntil([&]() {
    std::lock_guard lock(sent_mu_);
    for (const auto& payload : sent_payloads_) {
      if (payload.find("call_accept") != std::string::npos) {
        return true;
      }
    }
    return false;
  });
  bool accept_says_mobile = false;
  {
    std::lock_guard lock(sent_mu_);
    for (const auto& payload : sent_payloads_) {
      accept_says_mobile |= payload.find("call_accept") != std::string::npos &&
                            payload.find("mobility") != std::string::npos &&
                            payload.find("mobile") != std::string::npos;
    }
  }
  EXPECT_TRUE(accept_says_mobile) << "the accept advertises caps.mobility";

  const CallPathPolicy policy = stack_->PathPolicyFor(call_id);
  EXPECT_EQ(policy.relay_role, CallRelayRole::Anchor) << "mobile here + stationary there";
  EXPECT_FALSE(policy.upgrade_to_direct);
  EXPECT_EQ(policy.relay_role, DecideCallPathPolicy(MobilityClass::Stationary, MobilityClass::Mobile).relay_role)
      << "the other end computes the same";

  mesh_config_.mobility = "auto";
  stack_->ReloadMobilityOverride();
  AppRuntime::RunUIAndOwnerTasks();
  EXPECT_EQ(stack_->LocalMobility(), MobilityClass::Unknown) << "auto, no network seen in the test";
  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
}

// projects/privacy T1: a call with a peer outside the audience runs on the relay only — no direct
// dial, no punch, no move onto a direct link — whatever the mobility policy would allow.
TEST_F(CallUiBackendStackTest, PeerOutsideTheAudienceGetsARelayOnlyCall) {
  mesh::AddressDisclosurePolicy contacts_only;
  contacts_only.audience = mesh::DirectAudience::Contacts;
  disclosure_.Publish(contacts_only);
  const std::string call_id = "call:stranger";
  CallPeerCaps remote;
  remote.present = true;
  remote.mobility = MobilityClass::Stationary;
  ASSERT_TRUE(IngestInvite(call_id, remote));

  CallPathPolicy policy = stack_->PathPolicyFor(call_id);
  EXPECT_TRUE(policy.relay_only);
  EXPECT_FALSE(policy.punch_at_start);
  EXPECT_FALSE(policy.upgrade_to_direct);
  EXPECT_EQ(policy.relay_role, CallRelayRole::Anchor);

  contacts_only.contacts.insert("account:peer");
  disclosure_.Publish(contacts_only);
  policy = stack_->PathPolicyFor(call_id);
  EXPECT_FALSE(policy.relay_only) << "a contact may learn our address";
}

// k6: the invite's caps.mobility is recorded for the call — a mobile inviter anchors this end's
// policy on the relay although this end is unclassified.
TEST_F(CallUiBackendStackTest, InviteMobilitySetsTheCallsPolicy) {
  const std::string call_id = "call:remote-mobile";
  CallPeerCaps remote;
  remote.present = true;
  remote.mobility = MobilityClass::Mobile;
  ASSERT_TRUE(IngestInvite(call_id, remote));
  AppRuntime::RunUIAndOwnerTasks();
  ASSERT_EQ(stack_->LocalMobility(), MobilityClass::Unknown);
  const CallPathPolicy policy = stack_->PathPolicyFor(call_id);
  EXPECT_EQ(policy.relay_role, CallRelayRole::Anchor);
  EXPECT_FALSE(policy.punch_at_start);
  EXPECT_EQ(stack_->PathPolicyFor("call:other").relay_role, CallRelayRole::Standby) << "per call";
}

TEST_F(CallUiBackendStackTest, InviteAcceptMediaPathThroughBackend) {
  // BindTestMediaPath: full CallStack → Bridge → StartDuplex → ConnectAsync → Leave.
  const std::string call_id = "call:ui-media";
  ASSERT_TRUE(IngestInvite(call_id));
  ASSERT_TRUE(stack_->MediaKeys()->PutEpochKey(call_id, 1, TestMediaKey()));

  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);

  DrainUntil([&]() {
    return (stack_->Engine() && stack_->Engine()->IsActive() && stack_->Engine()->ActiveSessionId() == call_id) ||
           ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting ||
           ui_->Phase() == CallPhase::ConnectFailed;
  });
  ASSERT_TRUE(stack_->Engine());
  EXPECT_TRUE(stack_->Engine()->IsActive()) << "phase=" << CallPhaseName(ui_->Phase()) << " err=" << ui_->LastError();
  EXPECT_EQ(stack_->Engine()->ActiveSessionId(), call_id);
  // Peer reach settles on its lane, so the dial follows StartDuplex asynchronously.
  DrainUntil([&]() { return transport_->connect_async_calls >= 1 || ui_->Phase() == CallPhase::ConnectFailed; });
  EXPECT_GE(transport_->connect_async_calls, 1)
      << "phase=" << CallPhaseName(ui_->Phase()) << " err=" << ui_->LastError();
  EXPECT_TRUE(transport_->active);

  DrainUntil([&]() {
    return ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting || ui_->MediaChromeLive();
  });
  EXPECT_TRUE(ui_->Phase() == CallPhase::InCall || ui_->Phase() == CallPhase::MediaConnecting ||
              ui_->Phase() == CallPhase::JoinedLocal)
      << CallPhaseName(ui_->Phase());

  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value() && (!stack_->Engine() || !stack_->Engine()->IsActive());
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
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
  auto pending = ui_->TopPendingInvite();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(pending->has_value());
}

TEST_F(CallUiBackendStackTest, StartCallAndLeaveViaBackend) {
  Thread thread;
  // Windows: thread id is a directory name under threads/ — no ':' (illegal path char).
  thread.id = "thread-ui-out";
  thread.kind = ThreadKind::Direct;
  thread.title = "Peer";
  thread.updated_at = util::NowUnixMs();
  ASSERT_TRUE(store_->UpsertThread(thread));

  auto started = StartCallNow(*ui_, thread.id, false, {"account:peer"});
  ASSERT_TRUE(started) << started.error().message;
  EXPECT_EQ(ui_->Phase(), CallPhase::OutboundCalling);
  EXPECT_TRUE(stack_->WantEphemeralListen());

  auto active = ui_->ActiveLocalCall();
  ASSERT_TRUE(active && active->has_value());
  EXPECT_EQ((*active)->call_id, started->call_id);

  ui_->LeaveCall(started->call_id);
  ui_->Apply(CallLifecycleEvent::LeaveClicked, started->call_id);
  DrainUntil([&]() {
    auto after = ui_->ActiveLocalCall();
    return after && !after->has_value();
  });
  // LeaveClicked posts LeaveCall on Critical — flush before TearDown destroys the seat/CSM.
  EXPECT_TRUE(AppRuntime::DrainWorkersThenUI(std::chrono::milliseconds(2000)));
  EXPECT_EQ(ui_->Phase(), CallPhase::Idle);
}

TEST_F(CallUiBackendStackTest, RingChangesDoNotRebindCallStatePorts) {
  // B49: port sets used to be re-bound on every ring change (relay-receive thread) and every
  // accessor query while the bridge / CSM called them → SIGABRT in std::function::operator=.
  // Binding happens only at the owner's bind points (BuildSessions / BindMediaProducts).
  const int binds = stack_->CallStateBindsForTest();
  EXPECT_GT(binds, 0);
  for (int i = 0; i < 5; ++i) {
    ASSERT_NE(stack_->Calls(), nullptr);
  }
  const int desires_before = listen_desires_;
  ASSERT_TRUE(IngestInvite("call:b49-a"));
  ASSERT_TRUE(IngestInvite("call:b49-b"));
  DrainUntil([&]() { return listen_desires_ > desires_before; });
  EXPECT_GT(listen_desires_, desires_before) << "ring-change path did not run";
  EXPECT_EQ(stack_->CallStateBindsForTest(), binds);

  // Genuine teardown / recreate re-binds.
  stack_->PrepareForMeshStop({});
  stack_->FinishMeshStop();
  stack_->BindTestMediaPath(transport_.get(), dial_.get());
  ASSERT_NE(stack_->Calls(), nullptr);
  EXPECT_GT(stack_->CallStateBindsForTest(), binds);
  EXPECT_TRUE(ui_->Available());
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
