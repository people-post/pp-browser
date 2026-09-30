#include "app/node/tools/call_probe/ProductStackHarness.h"
#include "feature/calls/LocalNetworkReaction.h"
#include "feature/conversations/MeshConnectivityWiring.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/chat/RelayEnvelope.h"
#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallTypes.h"
#include "domain/people/ContactIdentity.h"
#include "domain/people/ContactTypes.h"
#include "domain/messaging/CallLifecycleTypes.h"
#include "common/directory/DirectoryJson.h"
#include "common/directory/DirectoryTypes.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/data/MeshRole.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/os/OsThreadStackDump.h"
#include "domain/people/MeshHopPolicy.h"
#include "domain/mesh/reachability/Reachability.h"
#include "common/thread/ThreadRecordTypes.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <thread>
#include <vector>

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

} // namespace

Roe<std::unique_ptr<ProductStackHarness>> ProductStackHarness::Create(
    std::unique_ptr<pp::amp::AmpStack> stack, std::shared_ptr<pp::adp::Clock> clock,
    std::string advertise_ma, const std::string& hop_ma, std::vector<std::string> extra_hops) {
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

  if (auto init = harness->InitStoresAndStack(hop_ma, extra_hops); !init) {
    harness->Shutdown();
    return init.error();
  }
  return harness;
}

ProductStackHarness::~ProductStackHarness() {
  Shutdown();
}

Roe<void> ProductStackHarness::InitStoresAndStack(const std::string& hop_ma,
                                                  const std::vector<std::string>& extra_hops) {
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
  // Further hops this probe knows (group phases, V050 gt6) — after the warm hop, in order.
  for (const std::string& ma : extra_hops) {
    if (!ma.empty() && ma != hop_ma) {
      app_config_.mesh.bootstrap_peers.push_back(ma);
    }
  }

  mesh_connectivity_ = std::make_unique<MeshConnectivity>();
  mesh_media_relay_ = std::make_unique<MeshMediaRelay>(*mesh_connectivity_);
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
  // Set before the stack starts and never changed afterwards: one snapshot serves every owner.
  deps.mesh_config = [cfg = std::make_shared<const MeshConfig>(app_config_.mesh)]() { return cfg; };
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
  MeshConnectivityWiringInputs reach;
  reach.mesh = deps.mesh;
  reach.contacts = deps.contacts;
  reach.mesh_config = deps.mesh_config;
  reach.list_directory_nodes = deps.list_directory_nodes;
  reach.list_dht_nodes = deps.list_dht_nodes;
  reach.seed_dial_ok = deps.seed_dial_ok;
  reach.register_direct_endpoint = deps.delivery.register_peer_direct_endpoint;
  mesh_connectivity_->SetDeps(MakeMeshConnectivityDeps(std::move(reach)));
  deps.connectivity = mesh_connectivity_.get();
  deps.media_relay = mesh_media_relay_.get();

  stack_->BuildSessions(deps);
  if (!ui_->Available()) {
    return Error("CallUiBackend unavailable after BuildSessions");
  }
  // Invite listen_multiaddrs are peer-private under dual-SNAT; filter before dial-book write
  // so the mesh media plane does not RegisterEndpoint undialable RFC1918 (HL004).
  stack_->RunOnOwner([this](CallSessionManager& calls) {
    calls.SetRegisterPeerListenMultiaddrs(
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
  });
  stack_->DetachMeshMedia();
  mesh_connectivity_->Wire();
  mesh_media_relay_->Wire();
  stack_->OnMeshServicesStarted();
  network_monitor_ = std::make_unique<NetworkMonitor>();
  if (!network_monitor_->Start([this](const NetworkChange& change) {
        ReactToNetworkChange(change, host_.get(), stack_.get());
      })) {
    std::cerr << "warning: product-stack network monitor unavailable" << std::endl;
  }

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
  if (mesh_connectivity_) {
    mesh_connectivity_->RefreshHopPolicy();  // contacts are rendezvous / punch-introducer candidates
  }
  if (stack_) {
    stack_->RunOnOwner([account_id, peer_id](CallSessionManager& calls) {
      calls.NoteMeshPeerIdForRelay(account_id, peer_id);
    });
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

void ProductStackHarness::RefreshMeshMedia() {
  broadcast_.reset();  // borrows the relay objects the rewire replaces
  stack_->DetachMeshMedia();
  mesh_media_relay_->ResetRelayClient();
  mesh_connectivity_->ResetDialRegistry();
  mesh_connectivity_->Wire();
  mesh_media_relay_->Wire();
  stack_->RebindMeshMedia();
}

Roe<void> ProductStackHarness::EnsurePeerCircuitPath(const std::string& peer_id) {
  if (!stack_ || peer_id.empty()) {
    return Error("circuit path: missing stack/peer");
  }
  // Async + PumpUntil: the completion lands on the UI mailbox.
  std::optional<Roe<void>> result;
  mesh_connectivity_->TryEnsurePeerReachableAsync(peer_id, [&](Roe<void> value) { result = std::move(value); });
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

Roe<void> ProductStackHarness::EnsureGroupOriginThread(const std::string& thread_id,
                                                       const std::vector<std::string>& peer_accounts) {
  Thread thread;
  thread.id = thread_id;
  thread.kind = ThreadKind::Group;
  thread.channel = ThreadChannel::E2ePublic;
  thread.title = "group call";
  thread.updated_at = util::NowUnixMs();
  thread.participant_contact_ids = {local_account_};
  thread.participant_contact_ids.insert(thread.participant_contact_ids.end(), peer_accounts.begin(),
                                        peer_accounts.end());
  if (auto up = store_->UpsertThread(thread); !up) {
    return up.error();
  }
  return {};
}

void ProductStackHarness::Pump() {
  // UI mailbox only — the mesh runs on MeshHost's MeshPump.
  AppRuntime::RunUITasks();
  if (!signal_dir_.empty()) {
    PollSignalInbox();
  }
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
  if (stack_) {
    stack_->RunOnOwner([account_id, peer_id](CallSessionManager& calls) {
      calls.NoteMeshPeerIdForRelay(account_id, peer_id);
    });
  }
}

Roe<void> ProductStackHarness::SendCallControl(const std::string& peer_account,
                                               const ThreadMessage& msg) {
  // Same contract as the product's SendUserMessage: prepare, enqueue, return — delivery completes
  // later (Amp on Mesh I/O). A blocking send here parked the calls owner on the peer's ack; after
  // hangup the peer was gone, quiesce ran out and teardown freed the stack under it (hard-w5).
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
  if (!signal_dir_.empty()) {
    auto written = WriteSignal(peer_account, env);
    control_sends_.fetch_add(1, std::memory_order_release);
    return written;
  }
  const std::string dial_key = AmpDialKeyForAccount(peer_account);
  if (!chat_ || dial_key.empty()) {
    control_sends_.fetch_add(1, std::memory_order_release);
    return Error(!chat_ ? "chat transport not started" : "missing amp dial key for call-control");
  }
  chat_->SendEnvelopeAsync(dial_key, env, [sends = &control_sends_, id = msg.id](Roe<void> sent) {
    if (!sent) {
      std::cerr << "warning: call-control send failed message_id=" << id << " err=" << sent.error().message
                << std::endl;
    }
    sends->fetch_add(1, std::memory_order_release);
  });
  return {};
}

void ProductStackHarness::SetSignalDir(std::filesystem::path dir) {
  signal_dir_ = std::move(dir);
  std::error_code ec;
  std::filesystem::create_directories(SignalInbox(local_account_), ec);
  std::cout << "ok  product-stack signaling via dir=" << signal_dir_.string() << " (no Amp chat path)\n";
}

std::filesystem::path ProductStackHarness::SignalInbox(const std::string& account) const {
  std::string name = account;
  std::replace_if(
      name.begin(), name.end(), [](char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '-'; },
      '_');
  return signal_dir_ / ("inbox-" + name);
}

Roe<void> ProductStackHarness::WriteSignal(const std::string& peer_account, const RelayEnvelope& env) {
  const std::filesystem::path inbox = SignalInbox(peer_account);
  std::error_code ec;
  std::filesystem::create_directories(inbox, ec);
  Object file;
  file.set("message_id", env.message_id);
  file.set("sender_relay_id", env.sender_relay_id);
  file.set("sender_contact_id", env.sender_contact_id);
  file.set("timestamp", static_cast<int64_t>(env.timestamp));
  file.set("payload_b64", env.body.e2e.payload_b64);
  std::ostringstream name;
  name << std::setw(16) << std::setfill('0') << util::NowUnixMs() << "-" << std::setw(6) << ++signal_seq_ << "-"
       << local_peer_id_.substr(0, 8);
  const std::filesystem::path tmp = inbox / (name.str() + ".tmp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out << DumpJson(file);
    if (!out) {
      return Error("signal write failed: " + tmp.string());
    }
  }
  // Rename is atomic on one filesystem: the reader never sees a partial file.
  std::filesystem::rename(tmp, inbox / (name.str() + ".json"), ec);
  if (ec) {
    return Error("signal rename failed: " + ec.message());
  }
  return {};
}

void ProductStackHarness::PollSignalInbox() {
  const auto now = std::chrono::steady_clock::now();
  if (now < next_signal_poll_) {
    return;
  }
  next_signal_poll_ = now + std::chrono::milliseconds(100);
  std::error_code ec;
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::directory_iterator(SignalInbox(local_account_), ec)) {
    if (entry.path().extension() == ".json") {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  for (const auto& path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    in.close();
    std::filesystem::remove(path, ec);
    auto obj = TryParseObject(buf.str());
    if (!obj) {
      continue;
    }
    RelayEnvelope env;
    env.message_id = obj->getString("message_id").value_or("");
    env.sender_relay_id = obj->getString("sender_relay_id").value_or("");
    env.sender_contact_id = obj->getString("sender_contact_id").value_or("");
    env.timestamp = static_cast<int64_t>(obj->getNonNegInt("timestamp").value_or(0));
    env.body.e2e.payload_b64 = obj->getString("payload_b64").value_or("");
    // Relay inbox ingestion runs off the UI thread in the product — do the same.
    AppRuntime::PostWorkerBackground([this, env = std::move(env)]() mutable { OnChatInbound(std::move(env)); });
  }
}

void ProductStackHarness::ForceDialMiss(const std::string& peer_id) {
  if (!host_ || !host_->Amp()) {
    return;
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  auto ok = std::make_shared<std::atomic<bool>>(false);
  host_->Amp()->Links().EnsureAssociation(peer_id, [done, ok](pp::amp::PeerLinkManager::LinkRoe r) {
    ok->store(static_cast<bool>(r), std::memory_order_release);
    done->store(true, std::memory_order_release);
  });
  // Short: a long private dial under dual-SNAT can outlive the hop mapping (~5 s LooksAlive).
  const bool finished = PumpUntil([&] { return done->load(std::memory_order_acquire); }, 3500);
  if (!finished) {
    host_->Amp()->Links().AbortInflightDial(peer_id);
  }
  std::cout << "ok  product-stack force-dial-fail peer=" << peer_id
            << (finished ? (ok->load() ? " (unexpectedly ok)" : " (miss; backoff left armed)")
                         : " (timed out; aborted)")
            << "\n";
}

Roe<void> ProductStackHarness::RegisterPeerPrivateEndpoint(const std::string& peer_id,
                                                           const std::string& multiaddr) {
  if (!host_ || !host_->Amp()) {
    return Error("mesh amp down");
  }
  if (auto reg = host_->Amp()->Links().RegisterEndpoint(peer_id, multiaddr); !reg) {
    return reg.error();
  }
  std::cout << "ok  product-stack dirty-book registered peer=" << peer_id << " ma=" << multiaddr << "\n";
  return {};
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

/**
 * Group calls: per-publisher RX over a sliding window. Met once `streams` remote streams each
 * decoded `frames` audio frames within `window_ms` — windowed, so a 1:1 stream left over from
 * before SoftMigrate stops counting once it goes quiet.
 */
class PublisherRxGate {
public:
  PublisherRxGate(const char* role, int streams, int frames, int window_ms)
      : role_(role), streams_(streams), frames_(static_cast<uint64_t>(std::max(frames, 1))),
        window_(std::chrono::milliseconds(std::max(window_ms, 500))) {}

  bool Enabled() const { return streams_ > 0; }
  bool Met() const { return met_; }
  std::chrono::steady_clock::time_point MetAt() const { return met_at_; }
  const std::string& Last() const { return last_; }

  void Tick(const std::vector<CallMediaStreamHealth>& streams) {
    const auto now = std::chrono::steady_clock::now();
    if (!Enabled() || met_ || now < next_sample_) {
      return;
    }
    next_sample_ = now + std::chrono::milliseconds(250);
    Sample sample{now, {}};
    for (const CallMediaStreamHealth& s : streams) {
      sample.rx[s.stream_id] = s.rx_frames;
    }
    samples_.push_back(std::move(sample));
    while (samples_.size() > 1 && now - samples_.front().at > window_) {
      samples_.pop_front();
    }
    const Sample& oldest = samples_.front();
    if (now - oldest.at < window_ - std::chrono::milliseconds(300)) {
      return;  // window not covered yet
    }
    std::ostringstream desc;
    int live = 0;
    for (const auto& [id, rx] : samples_.back().rx) {
      const auto base = oldest.rx.find(id);
      const uint64_t delta = rx - (base == oldest.rx.end() ? 0 : std::min(base->second, rx));
      desc << (desc.tellp() > 0 ? "," : "") << id << ":" << delta;
      if (delta >= frames_) {
        ++live;
      }
    }
    last_ = "streams=" + desc.str();
    if (live >= streams_) {
      met_ = true;
      met_at_ = now;
      std::cout << "ok  publisher rx gate " << role_ << " live=" << live << " (need " << streams_ << " x "
                << frames_ << " frames/" << window_.count() << "ms) " << last_ << "\n"
                << std::flush;
    }
  }

private:
  struct Sample {
    std::chrono::steady_clock::time_point at;
    std::map<uint32_t, uint64_t> rx;
  };

  const char* role_;
  int streams_;
  uint64_t frames_;
  std::chrono::milliseconds window_;
  std::deque<Sample> samples_;
  std::chrono::steady_clock::time_point next_sample_{};
  bool met_ = false;
  std::chrono::steady_clock::time_point met_at_{};
  std::string last_ = "streams=(none)";
};

} // namespace

std::vector<CallMediaStreamHealth> ProductStackHarness::RxStreams() const {
  if (!stack_ || !stack_->MediaEngine()) {
    return {};
  }
  return stack_->MediaEngine()->HealthSnapshot().streams;
}

std::optional<std::string> ProductStackHarness::MaybeAcceptPendingInvite(
    std::optional<std::chrono::steady_clock::time_point>& first_seen) {
  auto pending = ui_->TopPendingInvite();
  if (!pending || !pending->has_value()) {
    return std::nullopt;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!first_seen) {
    first_seen = now;
    if (accept_delay_ms_ > 0) {
      std::cout << "ok  product-stack invite seen; AcceptClicked in " << accept_delay_ms_ << " ms\n";
    }
  }
  if (now - *first_seen < std::chrono::milliseconds(accept_delay_ms_)) {
    return std::nullopt;
  }
  const std::string call_id = (*pending)->call_id;
  const std::string inviter = (*pending)->inviter_identity;
  // Reverse Accept rides Amp chat: ensure nested path to inviter PeerId (map from invite).
  const std::string inviter_peer = AmpDialKeyForAccount(inviter);
  // Signal-dir: Accept goes back through the inbox — media must reach the peer from cold.
  if (!UsesSignalDir() && !inviter_peer.empty() && inviter_peer != inviter) {
    if (!(host_ && host_->Amp() && host_->Amp()->Links().IsConnected(inviter_peer))) {
      if (auto path = EnsurePeerCircuitPath(inviter_peer); !path) {
        std::cerr << "warning: product-stack answerer circuit path: " << path.error().message << "\n";
      }
    }
  }
  ui_->Apply(CallLifecycleEvent::InviteSeen, call_id);
  ui_->Apply(CallLifecycleEvent::AcceptClicked, call_id);
  std::cout << "ok  product-stack AcceptClicked call_id=" << call_id << "\n";
  // Offerer arms wait-inbound only after CallAccept. If we Pump UI StartSfu first, inbound
  // hello lands on a pending bundle (offerer=0) and the PeerLink is torn down (HL004 race).
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  return call_id;
}

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
  std::string call_id;
  std::optional<std::chrono::steady_clock::time_point> invite_seen;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
  bool min_rx_met = false;
  bool was_in_call = false;
  MediaFlowMonitor flow("answerer", rx_stall_ms_, rx_watch_ms_);
  PublisherRxGate gate("answerer", gate_streams_, gate_frames_, gate_window_ms_);
  auto gates_ok = [&]() {
    if (min_rx_frames > 0 && !min_rx_met) {
      std::cerr << "error: answerer rx frames=" << RxAudioFrames() << " < min-rx-frames=" << min_rx_frames << "\n";
      return false;
    }
    if (gate.Enabled() && !gate.Met()) {
      std::cerr << "error: answerer publisher rx gate not met (need " << gate_streams_ << " streams) last "
                << gate.Last() << "\n";
      return false;
    }
    return true;
  };
  while (std::chrono::steady_clock::now() < deadline) {
    Pump();
    if (call_id.empty()) {
      if (auto accepted = MaybeAcceptPendingInvite(invite_seen)) {
        call_id = *accepted;
      }
    }
    if (min_rx_frames > 0 && static_cast<int>(RxAudioFrames()) >= min_rx_frames) {
      min_rx_met = true;
    }
    if (ui_->Phase() == CallPhase::ConnectFailed) {
      std::cerr << "error: product-stack answerer ConnectFailed err=" << ui_->LastError() << "\n";
      return 1;
    }
    if (!call_id.empty()) {
      // Stay until the offerer leaves: hanging up on first RX ended the offerer's call before its
      // own media phase once call_leave was actually delivered.
      if (rx_stall_ms_ > 0) {
        if (auto stall = flow.Tick(RxAudioFrames(), TxAudioFrames())) {
          std::cerr << "error: " << *stall << "\n";
          return 1;
        }
      }
      gate.Tick(RxStreams());
      if (gate.Met() && leave_after_gate_ms_ > 0 &&
          std::chrono::steady_clock::now() - gate.MetAt() >= std::chrono::milliseconds(leave_after_gate_ms_)) {
        const uint64_t rx_before_leave = RxAudioFrames();  // the engine stops on Leave
        LeaveAndFlush(call_id);
        std::cout << "ok  product-stack answerer left the live call rx_frames=" << rx_before_leave << "\n";
        return gates_ok() ? 0 : 1;
      }
      if (ui_->Phase() == CallPhase::InCall) {
        was_in_call = true;
      } else if (was_in_call && ui_->Phase() == CallPhase::Idle) {
        std::cout << "ok  product-stack answerer: offerer left rx_frames=" << RxAudioFrames() << "\n";
        return gates_ok() ? 0 : 1;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (call_id.empty()) {
    std::cerr << "error: product-stack answerer never saw invite\n";
    return 1;
  }
  if (!gates_ok()) {
    return 1;
  }
  LeaveAndFlush(call_id);
  std::cout << "ok  product-stack answerer leave rx_frames=" << RxAudioFrames() << "\n";
  return 0;
}

Roe<void> ProductStackHarness::RunOffererCall(const std::vector<std::string>& peer_accounts, int hold_ms,
                                              int timeout_ms) {
  if (peer_accounts.empty()) {
    return Error("product-stack offerer: no invitee");
  }
  const std::string thread_id = peer_accounts.size() > 1 ? "thread-probe-group" : "thread-probe-out";
  auto thr = peer_accounts.size() > 1 ? EnsureGroupOriginThread(thread_id, peer_accounts)
                                      : EnsureOriginThread(thread_id, peer_accounts.front());
  if (!thr) {
    return thr.error();
  }
  std::optional<Roe<CallSession>> started;
  ui_->StartCall(thread_id, false, peer_accounts, [&started](Roe<CallSession> result) { started = std::move(result); });
  if (!PumpUntil([&started]() { return started.has_value(); }, 10000)) {
    return Error("product-stack StartCall timed out");
  }
  if (!*started) {
    return started->error();
  }
  const std::string call_id = (*started)->call_id;
  std::cout << "ok  product-stack StartCall call_id=" << call_id << " invitees=" << peer_accounts.size() << "\n";

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
  PublisherRxGate gate("offerer", gate_streams_, gate_frames_, gate_window_ms_);
  const auto media_at = std::chrono::steady_clock::now();
  bool invited_later = false;
  while (std::chrono::steady_clock::now() < hold_deadline) {
    Pump();
    if (ui_->Phase() == CallPhase::ConnectFailed) {
      return Error(std::string("product-stack ConnectFailed during hold: ") + ui_->LastError());
    }
    if (auto stall = flow.Tick(RxAudioFrames(), TxAudioFrames())) {
      return Error(*stall);
    }
    gate.Tick(RxStreams());
    MaybeInviteLater(call_id, media_at, invited_later);
    if (gate.Met() && leave_after_gate_ms_ > 0 &&
        std::chrono::steady_clock::now() - gate.MetAt() >= std::chrono::milliseconds(leave_after_gate_ms_)) {
      break;  // heard everyone; leave while the others stay (initiator leave)
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (gate.Enabled() && !gate.Met()) {
    LeaveAndFlush(call_id);
    return Error("publisher rx gate not met (need " + std::to_string(gate_streams_) + " streams) last " +
                 gate.Last());
  }

  LeaveAndFlush(call_id);
  std::cout << "ok  product-stack LeaveClicked Idle\n";
  return {};
}

void ProductStackHarness::MaybeInviteLater(const std::string& call_id,
                                           std::chrono::steady_clock::time_point media_at, bool& sent) {
  if (sent || invite_later_account_.empty() ||
      std::chrono::steady_clock::now() - media_at < std::chrono::milliseconds(invite_later_ms_)) {
    return;
  }
  sent = true;
  ui_->InviteParticipant(call_id, invite_later_account_, [account = invite_later_account_](Roe<void> invited) {
    if (invited) {
      std::cout << "ok  product-stack invited later account=" << account << "\n";
    } else {
      std::cerr << "error: product-stack later invite failed: " << invited.error().message << "\n";
    }
  });
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
        os::DumpAllThreadStacks();
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

Roe<ByteVector> ProductStackHarness::DevicePublicKey() const {
  if (!identity_) {
    return Error("identity store not ready");
  }
  return identity_->GetDeviceMlDsaPublicKey();
}

Roe<void> ProductStackHarness::EnableBroadcast(
    std::function<std::optional<ByteVector>(const std::string& peer_id)> publisher_key) {
  auto chat = host_ ? host_->ChatDeps() : std::nullopt;
  if (!chat || !stack_) {
    return Error("broadcast needs the mesh host and call plane");
  }
  // Same serving wiring as MeshDeliveryOrchestrator::AttachAmpTransports.
  broadcast_server_ = std::make_unique<AmpBroadcastTransport>(chat->links, chat->io.io_pump, chat->io.post_worker,
                                                              chat->io.post_io, chat->io.post_after);
  broadcast_server_->SetPublisherKeyResolver(publisher_key);
  broadcast_server_->SetPublisherSecretResolver([this]() -> std::optional<ByteVector> {
    auto sk = identity_->GetDeviceMlDsaPrivateKey();
    return sk ? std::optional<ByteVector>(*sk) : std::nullopt;
  });
  broadcast_server_->Start();

  BroadcastMeshDeps deps;
  deps.links = &chat->links;
  deps.io = chat->io;
  deps.relay = mesh_media_relay_->RelayAttachPorts();
  deps.publisher_key = std::move(publisher_key);
  deps.put_program_key = [this](const std::string& program_id, const std::string& join_handle,
                                BroadcastProgramKey key) {
    AmpBroadcastTransport::LiveProgramKey live;
    live.publisher_peer_id = key.publisher_peer_id;
    live.media_key_bytes = std::move(key.media_key);
    live.media_epoch = key.media_epoch;
    live.hop_peer_id = key.hop_peer_id;
    broadcast_server_->PutLiveProgramKey(program_id, join_handle, std::move(live));
  };
  deps.clear_program_key = [this](const std::string& program_id, const std::string& join_handle) {
    broadcast_server_->ClearLiveProgramKey(program_id, join_handle);
  };
  deps.announce = [this](const BroadcastTipDraft& draft) -> Roe<void> {
    last_tip_ = draft;
    std::cout << "broadcast announce state=" << (draft.state == PeerAnnounceState::Live ? "live" : "ended")
              << " program=" << draft.program_id << " join=" << draft.join_handle << " hop=" << draft.hop_peer_id
              << std::endl;
    return {};
  };
  // Like the product hub: the announce runs on the harness's (UI) thread, which reads last_tip_.
  deps.post_announce = [](std::function<void()> task) { AppRuntime::PostUI(std::move(task)); };
  broadcast_devices_ = std::make_unique<MediaDeviceArbiter>(CreateNullMediaDeviceBackend());
  broadcast_ = BroadcastHub::ForMesh(std::move(deps), *broadcast_devices_);
  if (!broadcast_) {
    return Error("broadcast hub unavailable (media_relay not wired)");
  }
  return {};
}

void ProductStackHarness::ShutdownImpl() {
  if (network_monitor_) {
    network_monitor_->Stop();  // before anything it reaches goes away
    network_monitor_.reset();
  }
  // Broadcast borrows the mesh media plane's relay objects and this host's links: it goes first.
  if (broadcast_) {
    ShutdownStep("broadcast");
    broadcast_.reset();
  }
  if (broadcast_server_) {
    broadcast_server_->Stop();
    broadcast_server_.reset();
  }
  broadcast_devices_.reset();
  // Product quit order (THREADING.md § Shutdown order): abort call media → quiesce runtime →
  // mesh stop (joins MeshPump) → join runtime → free. Freeing the call stack while the
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
    // Same order as ConversationsHub::StopMesh (L015). Detach = destroy (product
    // DetachAmpTransports): nothing may outlive the Amp stack.
    MeshHost* host = host_.get();
    mesh_connectivity_->InvalidateAsyncOps();
    stack_->PrepareForMeshStop([host]() { host->AbortInflightCircuitRequests(); });
    mesh_media_relay_->ResetRelayClient();
    chat_.reset();
    host_->Stop();
    stack_->FinishMeshStop();
    mesh_connectivity_->ResetAfterMeshStop();
  } else if (host_) {
    host_->Stop();
  }
  chat_.reset();
  host_.reset();
  if (stack_) {
    ShutdownStep("call-stack-shutdown");
    stack_->Shutdown();
  }
  if (mesh_media_relay_) {
    mesh_media_relay_->Clear();
  }
  if (mesh_connectivity_) {
    mesh_connectivity_->Clear();
  }
  ShutdownStep("runtime");
  AppRuntime::Shutdown();
  ShutdownStep("free");
  ui_.reset();
  stack_.reset();
  mesh_media_relay_.reset();
  mesh_connectivity_.reset();
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
