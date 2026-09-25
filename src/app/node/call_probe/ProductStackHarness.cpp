#include "app/node/call_probe/ProductStackHarness.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/chat/RelayEnvelope.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallTypes.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactTypes.h"
#include "feature/calls/CallLifecycle.h"
#include "common/directory/DirectoryJson.h"
#include "common/directory/DirectoryTypes.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntime.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/mesh/reachability/Reachability.h"
#include "common/thread/ThreadRecordTypes.h"

#include <chrono>
#include <cstdlib>
#if defined(__linux__)
#include <csignal>
#include <cstdio>
#include <execinfo.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <iostream>
#include <optional>
#include <thread>

namespace pbr {
namespace call_probe {
namespace {

ByteVector ProbeDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa5 ^ static_cast<uint8_t>(i));
  }
  return dek;
}

ByteVector SharedSessionKey() {
  ByteVector key(32, 0x5a);
  return key;
}

std::optional<std::string> PeerIdFromMa(const std::string& ma) {
  const auto pos = ma.rfind("/p2p/");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  auto id = ma.substr(pos + 5);
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == '/')) {
    id.pop_back();
  }
  if (id.empty()) {
    return std::nullopt;
  }
  return id;
}

#if defined(__linux__)
std::atomic_flag g_stack_dump_busy = ATOMIC_FLAG_INIT;

void DumpThisThreadStack(int /*sig*/) {
  while (g_stack_dump_busy.test_and_set(std::memory_order_acquire)) {
  }
  void* frames[64];
  const int n = backtrace(frames, 64);
  char header[64];
  const int len = std::snprintf(header, sizeof(header), "--- thread %ld\n", static_cast<long>(syscall(SYS_gettid)));
  (void)!write(STDERR_FILENO, header, static_cast<size_t>(len));
  backtrace_symbols_fd(frames, n, STDERR_FILENO);
  g_stack_dump_busy.clear(std::memory_order_release);
}

/** Diagnostics for a stuck teardown: every thread prints its backtrace (addr2line on the host). */
void DumpAllThreadStacks() {
  void* warm[1];
  (void)backtrace(warm, 1); // load libgcc unwinder outside the signal handler
  std::signal(SIGUSR2, DumpThisThreadStack);
  const long self = static_cast<long>(syscall(SYS_gettid));
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
    const long tid = std::strtol(entry.path().filename().c_str(), nullptr, 10);
    if (tid > 0 && tid != self) {
      syscall(SYS_tgkill, static_cast<long>(getpid()), tid, SIGUSR2);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
}
#else
void DumpAllThreadStacks() {}
#endif

} // namespace

Roe<std::unique_ptr<ProductStackHarness>> ProductStackHarness::Create(
    std::unique_ptr<pp::amp::AmpStack> stack, std::shared_ptr<pp::adp::Clock> clock,
    std::string advertise_ma, const std::string& hop_ma) {
  if (!stack) {
    return Error("null AmpStack");
  }
  if (!clock) {
    return Error("null Amp clock");
  }
  AppRuntime::Initialize();
  AppRuntime::InitializeUI();

  auto harness = std::unique_ptr<ProductStackHarness>(new ProductStackHarness());
  harness->clock_ = std::move(clock);
  harness->advertise_ma_ = std::move(advertise_ma);
  harness->local_peer_id_ = stack->LocalPeerId();
  harness->shared_session_key_ = SharedSessionKey();

  harness->host_ = std::make_unique<MeshHost>();
  // Own MeshPump like the product: with the main thread as sole Amp driver, any main-thread wait
  // on work that needs mesh progress (capture send, parked worker) deadlocked (hard-w5 hang).
  if (auto attached = harness->host_->AttachAmpStack(std::move(stack), harness->advertise_ma_,
                                                     MeshHost::AttachDrive::MeshPump);
      !attached) {
    return attached.error();
  }
  if (auto* punch = harness->host_->AmpPunch()) {
    punch->SetLocalCandidateAddrs({harness->advertise_ma_});
  }

  if (auto init = harness->InitStoresAndStack(hop_ma); !init) {
    harness->Shutdown();
    return init.error();
  }
  return harness;
}

ProductStackHarness::~ProductStackHarness() {
  Shutdown();
}

Roe<void> ProductStackHarness::InitStoresAndStack(const std::string& hop_ma) {
  data_dir_ = std::filesystem::temp_directory_path() / ("pp_call_probe_stack_" + util::GenerateUuid());
  std::error_code ec;
  std::filesystem::remove_all(data_dir_, ec);
  std::filesystem::create_directories(data_dir_, ec);

  store_ = std::make_unique<SqliteThreadStore>(data_dir_.string());
  if (auto listed = store_->ListThreads(); !listed) {
    return listed.error();
  }
  if (auto store_dek = store_->SetDek(ProbeDek()); !store_dek) {
    return store_dek.error();
  }
  contacts_ = std::make_unique<ContactsStore>(data_dir_.string());
  identity_ = std::make_unique<IdentityStore>(data_dir_.string(), "call-probe");
  if (auto dek = identity_->SetDek(ProbeDek()); !dek) {
    return dek.error();
  }
  auto loaded = identity_->LoadOrCreate();
  if (!loaded) {
    return loaded.error();
  }
  local_account_ = loaded->account_id;

  psk_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), "call-probe");
  if (auto psk_dek = psk_->SetDek(ProbeDek()); !psk_dek) {
    return psk_dek.error();
  }

  app_config_ = AppConfig{};
  NormalizeMeshConfig(app_config_.mesh);
  if (!hop_ma.empty()) {
    app_config_.mesh.bootstrap_peers = {hop_ma};
  }

  stack_ = std::make_unique<CallStack>();
  if (auto stores = stack_->InitializeStores(store_->ProfileDbPath(), "call-probe"); !stores) {
    return stores.error();
  }
  if (auto mk = stack_->MediaKeys()->SetDek(ProbeDek()); !mk) {
    return mk.error();
  }

  ui_ = std::make_unique<CallUiBackend>(*stack_);

  CallStackDeps deps;
  deps.store = store_.get();
  deps.contacts = contacts_.get();
  deps.identity = identity_.get();
  deps.psk = psk_.get();
  deps.config = [this]() -> const AppConfig& { return app_config_; };
  deps.mesh = [this]() -> MeshHost* { return host_.get(); };
  deps.list_directory_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
  deps.list_dht_nodes = []() { return std::vector<MeshDirectoryNode>{}; };
  deps.seed_dial_ok = []() { return true; };
  deps.prefetch_peer_reachability = [](const std::string&) {};
  deps.sync_mobile_ephemeral_listen = []() {};
  deps.delivery.sync_inbox_from_wake = [](bool) {};
  deps.delivery.ensure_peer_session_key = [this](const std::string& /*peer*/) -> Roe<EnsuredE2ePublicSessionKey> {
    EnsuredE2ePublicSessionKey out;
    out.session_key = shared_session_key_;
    return out;
  };
  deps.delivery.register_peer_direct_endpoint = [this](const std::string& identity,
                                                      const std::string& multiaddr) {
    if (!host_ || !host_->Amp()) {
      return;
    }
    // Dual-SNAT: private listen MAs are not dialable across peers — registering them makes
    // AmpDirectChat IsPeerReachable(account) true and OpenChannel dials undialable LAN.
    const std::string ip = IpHostFromMultiaddrPrefix(multiaddr);
    if (IsPrivateIpv4(ip) || IsLikelyUndialableLanIpv4(ip)) {
      return;
    }
    (void)host_->Amp()->Links().RegisterEndpoint(identity, multiaddr);
    if (auto pid = PeerIdFromMa(multiaddr); pid && *pid != identity) {
      (void)host_->Amp()->Links().RegisterEndpoint(*pid, multiaddr);
    }
  };
  deps.delivery.send_user_message = [this](const std::string& thread_id, const std::string& text,
                                           const SendRelayOptions& options) -> Roe<ThreadMessage> {
    ThreadMessage msg;
    msg.id = util::GenerateUuid();
    msg.thread_id = thread_id;
    msg.text = text;
    msg.content_type = options.content_type.value_or(ChatContentType::System);
    msg.payload_json = options.payload_json.value_or("");
    msg.timestamp = util::NowUnixMs();
    msg.sender_contact_id = local_account_;

    // Resolve recipient from the call-control thread's peer identity (E2ePublic direct).
    std::string peer_account;
    if (auto thr = store_->GetThread(thread_id); thr && thr->has_value()) {
      peer_account = (*thr)->peer_identity_value;
    }
    if (peer_account.empty()) {
      return Error("call-control thread missing peer");
    }
    if (auto sent = SendCallControl(peer_account, msg); !sent) {
      return sent.error();
    }
    return msg;
  };
  deps.bind_call_control = [this](CallControlInboundPorts ports) { inbound_ = std::move(ports); };

  stack_->BuildSessions(deps);
  if (!ui_->Available()) {
    return Error("CallUiBackend unavailable after BuildSessions");
  }
  // Invite listen_multiaddrs are peer-private under dual-SNAT; filter before dial-book write
  // so CallMediaPlane does not RegisterEndpoint undialable RFC1918 (HL004).
  if (stack_->Calls()) {
    stack_->Calls()->SetRegisterPeerListenMultiaddrs(
        [this](const std::string& identity, const std::vector<std::string>& multiaddrs) {
          std::vector<std::string> dialable;
          dialable.reserve(multiaddrs.size());
          for (const std::string& ma : multiaddrs) {
            const std::string ip = IpHostFromMultiaddrPrefix(ma);
            if (IsPrivateIpv4(ip) || IsLikelyUndialableLanIpv4(ip)) {
              continue;
            }
            dialable.push_back(ma);
          }
          if (!dialable.empty()) {
            stack_->RegisterCallPeerListenMultiaddrs(identity, dialable);
          }
        });
  }
  stack_->OnMeshServicesStarted();

  auto chat_deps = host_->ChatDeps();
  if (!chat_deps) {
    return Error("MeshHost chat deps unavailable");
  }
  // Same wiring as MeshDeliveryOrchestrator (empty io_pump under MeshPump).
  chat_ = std::make_unique<AmpDirectChatTransport>(chat_deps->links, chat_deps->io.io_pump,
                                                   chat_deps->io.post_worker, chat_deps->io.post_io,
                                                   chat_deps->io.post_after);
  chat_->Start();
  chat_->SetInboundHandler([this](RelayEnvelope env) { OnChatInbound(std::move(env)); });

  std::cout << "ok  product-stack CallStack+CallUiBackend wired account=" << local_account_
            << " peer_id=" << local_peer_id_ << "\n";
  return {};
}

Roe<void> ProductStackHarness::UpsertPeerContact(const std::string& account_id,
                                                 const std::string& peer_id,
                                                 const std::string& multiaddr) {
  if (account_id.empty() || peer_id.empty()) {
    return Error("peer contact requires account+peer_id");
  }
  Contact contact;
  contact.id = util::GenerateUuid();
  contact.display_name = account_id;
  contact.ids.push_back({ContactIdKind::Account, account_id, true});
  contact.ids.push_back({ContactIdKind::PeerId, peer_id, true});
  if (!multiaddr.empty()) {
    contact.multiaddrs.push_back(multiaddr);
  }
  PromoteFlatFieldsToNested(contact);
  SyncContactMirrors(contact);
  if (auto up = contacts_->Upsert(contact); !up) {
    return up.error();
  }
  if (stack_ && stack_->Calls()) {
    stack_->Calls()->NoteMeshPeerIdForRelay(account_id, peer_id);
  }
  LearnAccountPeerId(account_id, peer_id);
  // Dual-SNAT: do not RegisterEndpoint private advertise MAs — that poisons dial book and
  // makes Amp chat try undialable LAN before circuit (dogfood / hard-w5).
  if (host_ && host_->Amp() && !multiaddr.empty()) {
    const std::string ip = IpHostFromMultiaddrPrefix(multiaddr);
    if (!IsPrivateIpv4(ip) && !IsLikelyUndialableLanIpv4(ip)) {
      (void)host_->Amp()->Links().RegisterEndpoint(account_id, multiaddr);
      (void)host_->Amp()->Links().RegisterEndpoint(peer_id, multiaddr);
    }
  }
  return {};
}

Roe<void> ProductStackHarness::EnsurePeerCircuitPath(const std::string& peer_id) {
  if (!stack_ || peer_id.empty()) {
    return Error("circuit path: missing stack/peer");
  }
  // Async + PumpUntil: the completion lands on the UI mailbox.
  std::optional<Roe<void>> result;
  stack_->TryEnsureCallMediaReachableAsync(peer_id, [&](Roe<void> value) { result = std::move(value); });
  if (!PumpUntil([&] { return result.has_value(); }, 30000)) {
    return Error("call-media circuit reach timed out");
  }
  if (!*result) {
    return result->error();
  }
  if (host_ && host_->Amp() && !host_->Amp()->Links().IsConnected(peer_id)) {
    return Error("circuit path: peer not connected after Ensure");
  }
  std::cout << "ok  product-stack circuit path Connected peer=" << peer_id << "\n";
  return {};
}

Roe<void> ProductStackHarness::EnsureOriginThread(const std::string& thread_id,
                                                  const std::string& peer_account) {
  Thread thread;
  thread.id = thread_id;
  thread.kind = ThreadKind::Direct;
  thread.channel = ThreadChannel::E2ePublic;
  thread.title = peer_account;
  thread.updated_at = util::NowUnixMs();
  thread.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  thread.peer_identity_value = peer_account;
  thread.participant_contact_ids = {local_account_, peer_account};
  if (auto up = store_->UpsertThread(thread); !up) {
    return up.error();
  }
  return {};
}

void ProductStackHarness::Pump() {
  // UI mailbox only — the mesh runs on MeshHost's MeshPump.
  AppRuntime::RunUITasks();
}

bool ProductStackHarness::PumpUntil(const std::function<bool()>& done, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    Pump();
    if (done()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  Pump();
  return done();
}

std::string ProductStackHarness::AmpDialKeyForAccount(const std::string& account_id) const {
  if (account_id.empty()) {
    return {};
  }
  // Prefer mesh PeerId: under dual-SNAT the nested circuit is keyed by PeerId, and chat
  // OpenChannel(account) would dial a private invite listen MA instead of reusing carrier.
  if (const auto it = account_to_peer_id_.find(account_id); it != account_to_peer_id_.end() &&
                                                              !it->second.empty()) {
    return it->second;
  }
  if (contacts_) {
    if (auto found = contacts_->FindByIdentity(account_id, ContactIdKind::Account);
        found && found->has_value()) {
      const std::string peer_id = PeerIdFromContact(**found);
      if (!peer_id.empty()) {
        return peer_id;
      }
    }
  }
  return account_id;
}

void ProductStackHarness::LearnAccountPeerId(const std::string& account_id,
                                             const std::string& peer_id) {
  if (account_id.empty() || peer_id.empty()) {
    return;
  }
  account_to_peer_id_[account_id] = peer_id;
  if (stack_ && stack_->Calls()) {
    stack_->Calls()->NoteMeshPeerIdForRelay(account_id, peer_id);
  }
}

Roe<void> ProductStackHarness::SendCallControl(const std::string& peer_account,
                                               const ThreadMessage& msg) {
  struct CountOnExit {
    std::atomic<int>& n;
    ~CountOnExit() { n.fetch_add(1, std::memory_order_release); }
  } count{control_sends_};
  if (!chat_) {
    return Error("chat transport not started");
  }
  const std::string dial_key = AmpDialKeyForAccount(peer_account);
  if (dial_key.empty()) {
    return Error("missing amp dial key for call-control");
  }
  Object body;
  body.set("thread_id", msg.thread_id);
  body.set("text", msg.text);
  body.set("content_type", static_cast<int64_t>(msg.content_type));
  body.set("payload_json", msg.payload_json);
  body.set("message_id", msg.id);
  const std::string json = DumpJson(body);
  RelayEnvelope env;
  env.message_id = msg.id;
  env.sender_relay_id = local_peer_id_;
  env.sender_contact_id = local_account_;
  env.timestamp = msg.timestamp;
  env.body.e2e.payload_b64 =
      Base64Encode(ByteVector(reinterpret_cast<const uint8_t*>(json.data()),
                              reinterpret_cast<const uint8_t*>(json.data()) + json.size()));
  return chat_->SendEnvelope(dial_key, env);
}

void ProductStackHarness::OnChatInbound(RelayEnvelope env) {
  if (env.body.e2e.payload_b64.empty() || !inbound_.apply_inbound_control) {
    return;
  }
  auto bytes = Base64Decode(env.body.e2e.payload_b64);
  if (!bytes) {
    return;
  }
  const std::string json(bytes->begin(), bytes->end());
  auto obj = TryParseObject(json);
  if (!obj) {
    return;
  }
  ThreadMessage msg;
  msg.id = obj->getString("message_id").value_or(env.message_id);
  msg.thread_id = obj->getString("thread_id").value_or("thread:call-control");
  msg.text = obj->getString("text").value_or("");
  msg.payload_json = obj->getString("payload_json").value_or("");
  msg.timestamp = env.timestamp > 0 ? env.timestamp : util::NowUnixMs();
  msg.sender_contact_id = env.sender_contact_id.empty() ? env.sender_relay_id : env.sender_contact_id;
  if (auto ct = obj->getNonNegInt("content_type")) {
    msg.content_type = static_cast<ChatContentType>(*ct);
  } else {
    msg.content_type = ChatContentType::System;
  }
  if (!CallControlCodec::IsCallControlMessage(msg)) {
    return;
  }
  // Learn inviter PeerId before Accept so AmpDialKey uses carrier-keyed PeerId (HL004).
  if (auto ctype = CallControlCodec::ControlTypeFromMessage(msg);
      ctype && *ctype == CallControlType::CallInvite) {
    if (auto payload = TryParseObject(msg.payload_json)) {
      const std::string detail = payload->getString("detail").value_or("");
      if (auto invite = CallControlCodec::DecodeInvite(detail); invite) {
        const std::string account =
            invite->inviter_identity.empty()
                ? (env.sender_contact_id.empty() ? env.sender_relay_id : env.sender_contact_id)
                : invite->inviter_identity;
        if (!invite->libp2p_peer_id.empty()) {
          LearnAccountPeerId(account, invite->libp2p_peer_id);
        } else if (auto pid = PeerIdFromMa(
                       invite->listen_multiaddrs.empty() ? "" : invite->listen_multiaddrs.front());
                   pid) {
          LearnAccountPeerId(account, *pid);
        }
      }
    }
  }
  const std::string sender =
      env.sender_contact_id.empty() ? env.sender_relay_id : env.sender_contact_id;
  if (auto applied = inbound_.apply_inbound_control(msg, sender, std::nullopt, std::nullopt); !applied) {
    std::cerr << "warning: product-stack inbound control: " << applied.error().message << "\n";
  }
}

namespace {

/**
 * Per-second rx/tx trace + one-way stall detector for long holds (dogfood 2026-09-24 16:17: a
 * relayed call lost caller→callee audio after ~8 s with every link still up).
 */
class MediaFlowMonitor {
public:
  /** `watch_ms` > 0: only judge stalls within this long after the first rx frame (the remote
   * leaving at the end of its hold also flattens rx). */
  MediaFlowMonitor(const char* role, int stall_ms, int watch_ms = 0)
      : role_(role), stall_ms_(stall_ms), watch_ms_(watch_ms), start_(std::chrono::steady_clock::now()),
        last_change_(start_), next_report_(start_) {}

  /** Returns an error message once rx has been flat for stall_ms after first audio. */
  std::optional<std::string> Tick(const uint64_t rx, const uint64_t tx) {
    const auto now = std::chrono::steady_clock::now();
    if (rx != last_rx_) {
      if (last_rx_ == 0) {
        first_rx_ = now;
      }
      last_rx_ = rx;
      last_change_ = now;
    }
    if (now >= next_report_) {
      std::cout << "flow " << role_ << " t=" << Ms(now - start_) / 1000.0 << "s rx=" << rx << " tx=" << tx
                << "\n" << std::flush;
      next_report_ = now + std::chrono::seconds(1);
    }
    const bool watching = watch_ms_ <= 0 || Ms(now - first_rx_) < watch_ms_;
    if (stall_ms_ > 0 && rx > 0 && watching && Ms(now - last_change_) >= stall_ms_) {
      return std::string("rx stall ") + role_ + ": rx=" + std::to_string(rx) + " flat since t=" +
             std::to_string(Ms(last_change_ - start_) / 1000.0) + "s (tx=" + std::to_string(tx) + ")";
    }
    return std::nullopt;
  }

private:
  static int64_t Ms(std::chrono::steady_clock::duration d) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
  }

  const char* role_;
  int stall_ms_;
  int watch_ms_;
  std::chrono::steady_clock::time_point first_rx_{};
  std::chrono::steady_clock::time_point start_;
  std::chrono::steady_clock::time_point last_change_;
  std::chrono::steady_clock::time_point next_report_;
  uint64_t last_rx_ = 0;
};

} // namespace

uint64_t ProductStackHarness::TxAudioFrames() const {
  if (!stack_ || !stack_->MediaEngine()) {
    return 0;
  }
  return stack_->MediaEngine()->HealthSnapshot().tx_audio_frames;
}

uint64_t ProductStackHarness::RxAudioFrames() const {
  if (!stack_ || !stack_->MediaEngine()) {
    return 0;
  }
  return stack_->MediaEngine()->HealthSnapshot().rx_audio_frames;
}

int ProductStackHarness::RunAnswererHold(int hold_seconds, int min_rx_frames) {
  bool accepted = false;
  std::string call_id;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
  bool min_rx_met = false;
  bool was_in_call = false;
  MediaFlowMonitor flow("answerer", rx_stall_ms_, rx_watch_ms_);
  while (std::chrono::steady_clock::now() < deadline) {
    Pump();
    if (!accepted) {
      auto pending = ui_->TopPendingInvite();
      if (pending && pending->has_value()) {
        call_id = (*pending)->call_id;
        const std::string inviter = (*pending)->inviter_identity;
        // Reverse Accept rides Amp chat: ensure nested path to inviter PeerId (map from invite).
        const std::string inviter_peer = AmpDialKeyForAccount(inviter);
        if (!inviter_peer.empty() && inviter_peer != inviter) {
          if (!(host_ && host_->Amp() && host_->Amp()->Links().IsConnected(inviter_peer))) {
            if (auto path = EnsurePeerCircuitPath(inviter_peer); !path) {
              std::cerr << "warning: product-stack answerer circuit path: " << path.error().message
                        << "\n";
            }
          }
        }
        ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
        ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
        accepted = true;
        std::cout << "ok  product-stack AcceptClicked call_id=" << call_id << "\n";
        // Offerer arms wait-inbound only after CallAccept. If we Pump UI StartSfu first, inbound
        // hello lands on a pending bundle (offerer=0) and the PeerLink is torn down (HL004 race).
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
    if (min_rx_frames > 0 && static_cast<int>(RxAudioFrames()) >= min_rx_frames) {
      min_rx_met = true;
    }
    if (ui_->Phase() == CallPhase::ConnectFailed) {
      std::cerr << "error: product-stack answerer ConnectFailed err=" << ui_->LastError() << "\n";
      return 1;
    }
    if (accepted) {
      // Stay until the offerer leaves: hanging up on first RX ended the offerer's call before its
      // own media phase once call_leave was actually delivered.
      if (rx_stall_ms_ > 0) {
        if (auto stall = flow.Tick(RxAudioFrames(), TxAudioFrames())) {
          std::cerr << "error: " << *stall << "\n";
          return 1;
        }
      }
      if (ui_->Phase() == CallPhase::InCall) {
        was_in_call = true;
      } else if (was_in_call && ui_->Phase() == CallPhase::Idle) {
        std::cout << "ok  product-stack answerer: offerer left rx_frames=" << RxAudioFrames() << "\n";
        return min_rx_met || min_rx_frames <= 0 ? 0 : 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (!accepted) {
    std::cerr << "error: product-stack answerer never saw invite\n";
    return 1;
  }
  if (min_rx_frames > 0 && !min_rx_met) {
    std::cerr << "error: answerer rx frames=" << RxAudioFrames() << " < min-rx-frames=" << min_rx_frames
              << "\n";
    return 1;
  }

  if (!call_id.empty()) {
    LeaveAndFlush(call_id);
  }
  std::cout << "ok  product-stack answerer leave rx_frames=" << RxAudioFrames() << "\n";
  return 0;
}

Roe<void> ProductStackHarness::RunOffererCall(const std::string& peer_account, int hold_ms,
                                              int timeout_ms) {
  const std::string thread_id = "thread-probe-out";
  if (auto thr = EnsureOriginThread(thread_id, peer_account); !thr) {
    return thr.error();
  }
  auto started = ui_->StartCall(thread_id, false, {peer_account});
  if (!started) {
    return started.error();
  }
  const std::string call_id = started->call_id;
  std::cout << "ok  product-stack StartCall call_id=" << call_id << " peer=" << peer_account << "\n";

  const bool reached = PumpUntil(
      [this]() {
        const auto phase = ui_->Phase();
        return phase == CallPhase::InCall || phase == CallPhase::MediaConnecting ||
               phase == CallPhase::ConnectFailed || ui_->MediaChromeLive();
      },
      timeout_ms);
  if (!reached) {
    return Error("product-stack timed out waiting for media phase");
  }
  if (ui_->Phase() == CallPhase::ConnectFailed) {
    return Error(std::string("product-stack ConnectFailed: ") + ui_->LastError());
  }
  std::cout << "ok  product-stack media phase=" << CallPhaseName(ui_->Phase())
            << " rx=" << RxAudioFrames() << "\n";

  const auto hold_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(hold_ms);
  MediaFlowMonitor flow("offerer", rx_stall_ms_);
  while (std::chrono::steady_clock::now() < hold_deadline) {
    Pump();
    if (ui_->Phase() == CallPhase::ConnectFailed) {
      return Error(std::string("product-stack ConnectFailed during hold: ") + ui_->LastError());
    }
    if (auto stall = flow.Tick(RxAudioFrames(), TxAudioFrames())) {
      return Error(*stall);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  LeaveAndFlush(call_id);
  std::cout << "ok  product-stack LeaveClicked Idle\n";
  return {};
}

void ProductStackHarness::LeaveAndFlush(const std::string& call_id) {
  // LeaveCall sends call_leave after the UI is already Idle; returning (and Shutdown resetting
  // chat_) before that raced the send — the peer never saw the hangup and hung to timeout.
  const int sends_before = control_sends_.load(std::memory_order_acquire);
  ArmTeardownWatchdog();
  ShutdownStep("leave-clicked");
  ui_->Apply(CallLifecycleEvent::LeaveClicked, call_id);
  const bool flushed = PumpUntil(
      [this, sends_before]() {
        return ui_->Phase() == CallPhase::Idle &&
               (!stack_->MediaEngine() || !stack_->MediaEngine()->IsActive()) &&
               control_sends_.load(std::memory_order_acquire) > sends_before;
      },
      8000);
  std::cerr << "probe leave flushed=" << flushed << " phase=" << CallPhaseName(ui_->Phase()) << std::endl;
}

void ProductStackHarness::ShutdownStep(const char* step) {
  shutdown_step_.store(step, std::memory_order_release);
  std::cerr << "probe shutdown: " << step << std::endl;
}

void ProductStackHarness::Shutdown() {
  if (!host_ && !stack_ && !ui_ && data_dir_.empty()) {
    DisarmTeardownWatchdog();
    return;
  }
  // A blocked teardown must fail the probe, not hang the lab (the smoke has no exec timeout).
  ArmTeardownWatchdog();
  ShutdownImpl();
  DisarmTeardownWatchdog();
}

void ProductStackHarness::ArmTeardownWatchdog() {
  if (teardown_watchdog_.joinable()) {
    return;
  }
  teardown_done_.store(false, std::memory_order_release);
  teardown_watchdog_ = std::thread([this]() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while (!teardown_done_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::cerr << "error: probe teardown stuck at " << shutdown_step_.load(std::memory_order_acquire)
                  << std::endl;
        DumpAllThreadStacks();
        std::error_code ec;
        if (std::filesystem::is_directory("/share", ec)) {
          std::filesystem::current_path("/share", ec);
        }
        std::abort();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
}

void ProductStackHarness::DisarmTeardownWatchdog() {
  teardown_done_.store(true, std::memory_order_release);
  if (teardown_watchdog_.joinable()) {
    teardown_watchdog_.join();
  }
}

void ProductStackHarness::ShutdownImpl() {
  // Product quit order (THREADING.md § Shutdown order): abort call media → quiesce runtime →
  // CallStack::StopMesh (joins MeshPump) → join runtime → free. Freeing the call stack while the
  // mesh / workers still ran segfaulted in hard-w5 (2026-09-25).
  if (ui_ && stack_ && stack_->HasActiveLocalCall()) {
    if (auto active = ui_->ActiveLocalCall(); active && active->has_value()) {
      LeaveAndFlush((*active)->call_id);
    }
  }
  if (stack_) {
    ShutdownStep("abort-call-media");
    stack_->AbortCallMediaForShutdown();
  }
  ShutdownStep("quiesce");
  if (!AppRuntime::QuiesceForTeardown(std::chrono::milliseconds(2000))) {
    std::cerr << "warning: probe teardown quiesce budget exceeded" << std::endl;
  }
  ShutdownStep("stop-mesh");
  if (stack_ && host_) {
    // Detach = destroy (product DetachAmpTransports): nothing may outlive the Amp stack.
    stack_->StopMesh(*host_, [this]() { chat_.reset(); });
  } else if (host_) {
    host_->Stop();
  }
  chat_.reset();
  host_.reset();
  if (stack_) {
    ShutdownStep("call-stack-shutdown");
    stack_->Shutdown();
  }
  ShutdownStep("runtime");
  AppRuntime::Shutdown();
  ShutdownStep("free");
  ui_.reset();
  stack_.reset();
  if (psk_) {
    psk_->ClearDek();
  }
  psk_.reset();
  identity_.reset();
  contacts_.reset();
  store_.reset();
  if (!data_dir_.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(data_dir_, ec);
    data_dir_.clear();
  }
  AppRuntime::ShutdownUI();
  ShutdownStep("done");
}

} // namespace call_probe
} // namespace pbr
