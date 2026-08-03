#include "feature/messaging/MessagingHub.h"

#include "feature/messaging/ContactReachability.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/GroupMembershipService.h"
#include "feature/messaging/MobileEphemeralListenGate.h"
#include "feature/messaging/RelayDirectoryKemKeyResolver.h"
#include "feature/messaging/RelayDirectorySigningKeyResolver.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include "feature/messaging/PushDeviceCoordinator.h"
#include "feature/ai/AgentSession.h"
#include "base/crypto/ProfileSecretsService.h"
#include "base/data/LlmPreset.h"
#include "base/error/AppError.h"
#include "base/data/Libp2pRole.h"
#include "base/data/SessionStore.h"
#include "base/data/UserPreferences.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/GroupTypes.h"
#include "base/net/HttpClient.h"
#include "base/net/RegistrationClientUtil.h"
#include "base/people/ContactTypes.h"
#include "base/platform/AppLifecycle.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/NetworkConnectivity.h"
#include "base/platform/Platform.h"
#include "base/data/PlatformDefaults.h"
#include "libp2p/integration/host/AdvertisedAddrPublisher.h"
#include "libp2p/integration/host/CircuitBridgeTarget.h"
#include "libp2p/integration/host/DialBackService.h"
#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/CallMediaDirectService.h"
#include "libp2p/integration/host/IdentifyIntegrationService.h"
#include "libp2p/integration/host/LanMdnsDiscovery.h"
#include "libp2p/integration/host/MediaRelayService.h"
#include "base/people/MeshHopPolicy.h"
#include "libp2p/integration/host/NatTraversal.h"
#include "libp2p/integration/host/Reachability.h"
#include "common/StartupTiming.h"
#include "common/Utilities.h"

#include <SDL3/SDL_timer.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unordered_set>

namespace pbr {

namespace {

PeerSessionConfig SessionConfigFromApp(const AppConfig& config) {
  PeerSessionConfig session = MakePeerSessionConfig(
      config.libp2p.max_connections, config.libp2p.max_concurrent_dials, config.libp2p.dial_timeout_ms,
      config.libp2p.idle_ttl_ms, config.libp2p.dial_failure_backoff_ms);
  if (Platform::IsMobile()) {
    session.max_connections = std::min(session.max_connections, size_t{16});
    session.max_concurrent_dials = std::min(session.max_concurrent_dials, size_t{4});
    if (session.idle_ttl > std::chrono::milliseconds(120000)) {
      session.idle_ttl = std::chrono::milliseconds(120000);
    }
  }
  return session;
}

std::string MaskBriefLlmApiKey(const std::string& key) {
  constexpr const char kPrefix[] = "brf_llm_";
  if (key.empty()) {
    return "";
  }
  if (key.size() <= sizeof(kPrefix) - 1 + 4) {
    return std::string(kPrefix) + "••••";
  }
  return key.substr(0, sizeof(kPrefix) - 1 + 4) + "••••";
}

std::string FormatExpiresDisplay(const std::string& iso) {
  if (iso.size() >= 10) {
    return iso.substr(0, 10);
  }
  return iso;
}

void FillRegistrationFields(ProfileIdentityView& view, const LocalIdentity& identity) {
  const RegistrationStatus status = ClassifyRegistration(identity);
  view.registered = identity.registered ? "yes" : "no";
  view.registration_status = RegistrationStatusLabel(status);
  view.registration_expires =
      identity.registration_expires_at.empty() ? "" : FormatExpiresDisplay(identity.registration_expires_at);
  view.register_label = RegistrationActionLabel(status);
  view.show_register = true;
  view.show_rotate = (status == RegistrationStatus::Active || status == RegistrationStatus::ExpiringSoon) &&
                     !identity.brief_llm_api_key.empty();
  view.brief_llm_key_masked = MaskBriefLlmApiKey(identity.brief_llm_api_key);
}

bool MobileInCallRelayEligible(const CallSessionManager* call_sessions) {
  if (call_sessions == nullptr) {
    return false;
  }
  auto active = call_sessions->ActiveLocalCall();
  if (!active || !active->has_value()) {
    return false;
  }
  auto joined = call_sessions->ListJoinedParticipants((*active)->call_id);
  return joined && joined->size() >= 3;
}

MobileEphemeralListenInput BuildMobileEphemeralListenInput(bool messaging_ready, bool node_runtime_running,
                                                           bool ephemeral_active, bool active_local_call) {
  MobileEphemeralListenInput in;
  in.is_mobile = Platform::IsMobile();
  in.messaging_ready = messaging_ready;
  in.node_runtime_running = node_runtime_running;
  // Unknown must not flap-stop an active N025 listen (JNI/capability probe blips on Samsung).
  const NetworkTransport transport = ActiveNetworkTransport();
  // Treat Unknown as wifi for start too — Samsung JNI probes often report Unknown and
  // previously blocked N025 listen (then AcceptInvite sat behind a dead listen desire).
  in.on_wifi = transport == NetworkTransport::Wifi || transport == NetworkTransport::Unknown;
  in.foreground = AppLifecycle::IsForeground();
  in.active_local_call = active_local_call;
  in.ephemeral_active = ephemeral_active;
  return in;
}

} // namespace

MessagingHub::MessagingHub() {
  redirectLogger("MessagingHub");
}

MessagingHub::~MessagingHub() {
  Shutdown();
}

void MessagingHub::WireRelayAuthSigner() {
  if (!identity_) {
    return;
  }
  auto signer = [this](const std::vector<uint8_t>& sign_bytes) -> Roe<std::string> {
    return identity_->SignBytes(sign_bytes);
  };
  if (http_relay_) {
    http_relay_->SetAuthSigner(signer);
  }
  if (http_push_devices_) {
    http_push_devices_->SetAuthSigner(signer);
  }
}

void MessagingHub::UpdateServiceClients(const AppConfig& config) {
  const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
  const std::string relay_url =
      config.relay.base_url.empty() ? defaults.relay.base_url : config.relay.base_url;
  const std::string directory_url =
      config.directory.base_url.empty() ? defaults.directory.base_url : config.directory.base_url;
  const std::string registration_url = config.registration.base_url.empty()
                                           ? defaults.registration.base_url
                                           : config.registration.base_url;

  if (!relay_url.empty()) {
    if (!http_relay_ || http_relay_url_ != relay_url) {
      http_relay_url_ = relay_url;
      http_relay_ = std::make_unique<HttpRelayClient>(http_relay_url_);
      http_push_devices_ = std::make_unique<HttpPushDeviceClient>(http_relay_url_);
      http_client_compat_ = std::make_unique<HttpClientCompatClient>(http_relay_url_);
      WireRelayAuthSigner();
    }
    relay_ = http_relay_.get();
    push_devices_ = http_push_devices_.get();
    client_compat_ = http_client_compat_.get();
  } else {
    http_relay_.reset();
    http_push_devices_.reset();
    http_client_compat_.reset();
    http_relay_url_.clear();
    relay_ = nullptr;
    push_devices_ = nullptr;
    client_compat_ = nullptr;
    log().warning << "relay.base_url is empty; relay client disabled";
  }

  if (!directory_url.empty()) {
    if (!http_directory_ || http_directory_url_ != directory_url) {
      http_directory_url_ = directory_url;
      http_directory_ = std::make_unique<HttpDirectoryClient>(http_directory_url_);
    }
    directory_ = http_directory_.get();
  } else {
    http_directory_.reset();
    http_directory_url_.clear();
    directory_ = nullptr;
    log().warning << "directory.base_url is empty; directory client disabled";
  }

  if (!registration_url.empty()) {
    if (!http_registration_ || http_registration_url_ != registration_url) {
      http_registration_url_ = registration_url;
      http_registration_ = std::make_unique<HttpRegistrationClient>(http_registration_url_);
    }
    registration_ = http_registration_.get();
  } else {
    http_registration_.reset();
    http_registration_url_.clear();
    registration_ = nullptr;
    log().warning << "registration.base_url is empty; registration client disabled";
  }
}

void MessagingHub::InstallServiceClients(const AppConfig& config) {
  UpdateServiceClients(config);
}

Roe<void> MessagingHub::StartLibp2p(const AppConfig& config) {
  StartupPhase phase("MessagingHub::StartLibp2p");
  StopLibp2p();
  libp2p_last_error_.clear();

  Libp2pConfig libp2p_cfg = config.libp2p;
  NormalizeLibp2pConfig(libp2p_cfg);
  const Libp2pRole role = ResolveLibp2pRole(libp2p_cfg);

  NodeRuntimeConfig runtime;
  runtime.host.listen_enabled = (role == Libp2pRole::Node);
  runtime.host.listen_multiaddr = libp2p_cfg.listen_multiaddr;
  if (auto priv = identity_->GetEd25519PrivateKey()) {
    runtime.host.ed25519_private_key = *priv;
  }
  if (auto pub = identity_->GetEd25519PublicKey()) {
    runtime.host.ed25519_public_key = *pub;
  }
  runtime.sessions = SessionConfigFromApp(config_);
  runtime.bootstrap_peers = libp2p_cfg.bootstrap_peers;
  if (runtime.host.listen_enabled) {
    runtime.listen_candidates =
        BuildLibp2pListenCandidates(libp2p_cfg.listen_multiaddr, ListenBusyPolicy::DesktopFallback);
    AppendIpv6ListenCandidatesForPreferred(libp2p_cfg.listen_multiaddr, runtime.listen_candidates);
  }

  node_runtime_ = std::make_unique<NodeRuntime>();
  if (!worker_pool_) {
    return Error("MessagingHub worker pool not set (Application must call SetWorkerPool)");
  }
  runtime.worker_pool = worker_pool_;
  auto started = node_runtime_->Start(runtime);
  if (!started) {
    libp2p_last_error_ = node_runtime_->LastError().empty() ? started.error().message : node_runtime_->LastError();
    node_runtime_.reset();
    return started.error();
  }

  if (runtime.host.listen_enabled) {
    const std::string bound = node_runtime_->BoundListenMultiaddr();
    if (!bound.empty() && bound != config_.libp2p.listen_multiaddr) {
      config_.libp2p.listen_multiaddr = bound;
      if (session_store_ && session_store_->IsInitialized()) {
        if (auto saved = session_store_->SaveConfig(config_); !saved) {
          log().warning << "Failed to persist libp2p listen multiaddr: " << saved.error().message;
        } else {
          log().info << "libp2p listening on " << bound;
        }
      } else {
        log().info << "libp2p listening on " << bound << " (session store not ready to persist)";
      }
    }
  }
  StartMeshServices(role);
  return {};
}

void MessagingHub::StartMeshServices(Libp2pRole role) {
  if (!node_runtime_ || !node_runtime_->IsRunning()) {
    return;
  }

  dial_back_ = std::make_unique<DialBackService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  dial_back_->Start();

  circuit_relay_ = std::make_unique<CircuitRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.circuit_relay) {
    circuit_relay_->Start();
  }

  // Outbound client API always available; inbound hosting only when Node + capability.
  media_relay_ = std::make_unique<MediaRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  media_relay_->SetBudget(config_.libp2p.media_relay_budget);
  media_relay_->SetPricing(config_.libp2p.pricing.media_relay);
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.media_relay) {
    media_relay_->Start();
  }

  call_media_direct_ = std::make_unique<CallMediaDirectService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  call_media_direct_->Start();

  lan_mdns_ = std::make_unique<LanMdnsDiscovery>();
  lan_mdns_->SetOnDiscovered([this](const LanMdnsDiscoveredPeer& peer) { OnLanMdnsPeerDiscovered(peer); });
  if (auto started = lan_mdns_->Start(); !started) {
    log().warning << "LAN mDNS discovery unavailable: " << started.error().message;
  }

  ApplyMeshAdmissionPolicies();

  reachability_.SetOnUpdated([this]() {
    ApplyMeshAdmissionPolicies();
    PublishNodeAdvertisedAddrs();
    RegisterContactEndpoints();
    if (on_reachability_updated_) {
      on_reachability_updated_();
    }
  });

  if (role == Libp2pRole::Node) {
    const bool try_upnp = !upnp_auto_tried_;
    upnp_auto_tried_ = true;
    reachability_.StartProbe(*node_runtime_, *dial_back_, try_upnp);
  }
  WireCallMediaRelayDeps();
  PublishNodeAdvertisedAddrs();
}

void MessagingHub::PublishNodeAdvertisedAddrs() {
  if (!node_runtime_ || !node_runtime_->Host() || !node_runtime_->Identify()) {
    return;
  }
  const bool node = ResolveLibp2pRole(config_.libp2p) == Libp2pRole::Node;
  const bool publish = node && config_.libp2p.capabilities.media_relay;
  if (!publish) {
    return;
  }
  std::string peer_id;
  if (auto local = node_runtime_->Host()->LocalPeerIdBase58()) {
    peer_id = *local;
  }
  PublishAdvertisedListenSet(*node_runtime_->Host(), *node_runtime_->Identify(),
                             reachability_.Snapshot(), node_runtime_->BoundListenMultiaddr(), peer_id,
                             publish);
}

bool MessagingHub::HasActiveLocalCall() {
  if (call_lifecycle_ && call_lifecycle_->WantEphemeralListen()) {
    return true;
  }
  if (ephemeral_listen_desired_) {
    return true;
  }
  if (!call_sessions_) {
    return false;
  }
  if (auto active = call_sessions_->ActiveLocalCall(); active && active->has_value()) {
    return true;
  }
  return false;
}

void MessagingHub::PublishMobileCallScopedAddrs() {
  if (!node_runtime_ || !node_runtime_->EphemeralListenActive() || !node_runtime_->Host() ||
      !node_runtime_->Identify()) {
    return;
  }
  std::string peer_id;
  if (auto local = node_runtime_->Host()->LocalPeerIdBase58()) {
    peer_id = *local;
  }
  if (peer_id.empty()) {
    return;
  }
  const std::vector<std::string> advertised =
      BuildMobileCallScopedAdvertisedAddrs(node_runtime_->BoundListenMultiaddr(), peer_id);
  if (advertised.empty()) {
    return;
  }
  IdentifyIntegrationService* identify = node_runtime_->Identify();
  node_runtime_->Host()->Post([advertised, identify]() { (void)identify->PublishSelfAdvertisedAddrs(advertised); });
}

void MessagingHub::SyncMobileEphemeralListen() {
  if (!Platform::IsMobile() || !messaging_ready_ || !node_runtime_) {
    return;
  }

  // Lifecycle owns whether we want listen; tick only executes start/stop.
  const bool active_call = ephemeral_listen_desired_ ||
                           (call_lifecycle_ && call_lifecycle_->WantEphemeralListen());
  const MobileEphemeralListenInput gate = BuildMobileEphemeralListenInput(
      messaging_ready_, node_runtime_->IsRunning(), node_runtime_->EphemeralListenActive(), active_call);

  if (ShouldStartMobileEphemeralListen(gate)) {
    if (mobile_ephemeral_start_inflight_ || mobile_ephemeral_stop_inflight_) {
      // Watchdog: ListenOnAsync / IO completion can leave inflight stuck and block all retries.
      constexpr int64_t kEphemeralStartWatchdogMs = 12000;
      const int64_t started_at = mobile_ephemeral_start_inflight_at_ms_;
      const int64_t now_ms = util::NowUnixMs();
      if (mobile_ephemeral_start_inflight_ && started_at > 0 &&
          now_ms - started_at > kEphemeralStartWatchdogMs) {
        log().warning << "Mobile ephemeral listen inflight watchdog reset after "
                      << (now_ms - started_at) << "ms";
        mobile_ephemeral_start_inflight_ = false;
        mobile_ephemeral_start_inflight_at_ms_ = 0;
      } else {
        log().warning << "Mobile ephemeral listen start deferred (inflight start="
                      << (mobile_ephemeral_start_inflight_ ? 1 : 0)
                      << " stop=" << (mobile_ephemeral_stop_inflight_ ? 1 : 0) << ")";
        return;
      }
    }
    BrowserThread::ResumeIO();
    // StartEphemeralListenAsync only posts onto the libp2p io thread — do NOT queue it behind
    // Browser IO Prefetch/RequestBridge (Samsung: AcceptInvite never reached IO enter).
    if (!messaging_ready_ || !node_runtime_ || !node_runtime_->Host()) {
      return;
    }
    mobile_ephemeral_start_inflight_ = true;
    mobile_ephemeral_start_inflight_at_ms_ = util::NowUnixMs();
    log().warning << "Mobile ephemeral listen begin (async N025)";
    node_runtime_->StartEphemeralListenAsync([this](Roe<void> started) {
      // Libp2p io thread. Clear inflight on UI immediately — never wait for Browser IO Wire
      // (Samsung: listen succeeded but "started" never logged while PollInbox held IO).
      const std::string bound =
          (started && node_runtime_) ? node_runtime_->BoundListenMultiaddr() : std::string{};
      const std::string err = started ? std::string{} : started.error().message;
      BrowserThread::PostTask(BrowserThreadId::UI, [this, bound, err]() {
        mobile_ephemeral_start_inflight_ = false;
        mobile_ephemeral_start_inflight_at_ms_ = 0;
        if (!err.empty()) {
          if (err != mobile_ephemeral_last_start_error_) {
            mobile_ephemeral_last_start_error_ = err;
            log().warning << "Mobile ephemeral listen failed: " << mobile_ephemeral_last_start_error_;
          }
          return;
        }
        mobile_ephemeral_last_start_error_.clear();
        log().warning << "Mobile ephemeral listen started (N025) bound=" << bound;
      });
      if (!started) {
        return;
      }
      // Wire / mDNS on a worker — must not sit behind PollInbox on Browser IO.
      std::thread([this]() {
        if (!messaging_ready_ || !node_runtime_) {
          return;
        }
        const NetworkTransport transport = ActiveNetworkTransport();
        const bool on_wifi =
            transport == NetworkTransport::Wifi || transport == NetworkTransport::Unknown;
        const bool still_want =
            (ephemeral_listen_desired_ ||
             (call_lifecycle_ && call_lifecycle_->WantEphemeralListen())) &&
            AppLifecycle::IsForeground() && on_wifi;
        if (!still_want) {
          if (node_runtime_->EphemeralListenActive()) {
            node_runtime_->StopEphemeralListenAsync([this]() {
              std::thread([this]() {
                if (messaging_ready_) {
                  ApplyMeshAdmissionPolicies();
                  WireCallMediaRelayDeps();
                  SyncLanMdnsAdvertisement();
                }
              }).detach();
            });
          }
          return;
        }
        ApplyMeshAdmissionPolicies();
        WireCallMediaRelayDeps();
        PublishMobileCallScopedAddrs();
        SyncLanMdnsAdvertisement();
        if (media_relay_ && !mobile_ephemeral_relay_started_ &&
            MobileInCallRelayEligible(call_sessions_.get())) {
          media_relay_->Start();
          mobile_ephemeral_relay_started_ = true;
        }
      }).detach();
    });
    return;
  }

  if (!gate.ephemeral_active && gate.active_local_call && !mobile_ephemeral_start_inflight_) {
    // Visible skip reason when ring/outbound wants listen but gate refuses (wifi/fg).
    static std::string last_skip;
    std::string skip = std::string("wifi=") + (gate.on_wifi ? "1" : "0") +
                       " fg=" + (gate.foreground ? "1" : "0") +
                       " ready=" + (gate.messaging_ready ? "1" : "0") +
                       " node=" + (gate.node_runtime_running ? "1" : "0");
    if (skip != last_skip) {
      last_skip = skip;
      log().warning << "Mobile ephemeral listen not started (" << skip << ")";
    }
  }

  if (ShouldStopMobileEphemeralListen(gate)) {
    if (mobile_ephemeral_stop_inflight_ || mobile_ephemeral_start_inflight_) {
      return;
    }
    // StopListening PostAndWaits on the libp2p io thread — never block the UI tick
    // (Samsung: UI stuck in futex → Accept clicks never reach call_accept).
    mobile_ephemeral_stop_inflight_ = true;
    BrowserThread::PostTask(BrowserThreadId::IO, [this]() {
      if (mobile_ephemeral_relay_started_ && media_relay_) {
        media_relay_->Stop();
        mobile_ephemeral_relay_started_ = false;
      }
      auto finish = [this]() {
        if (messaging_ready_) {
          ApplyMeshAdmissionPolicies();
          WireCallMediaRelayDeps();
          SyncLanMdnsAdvertisement();
        }
        BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
          mobile_ephemeral_stop_inflight_ = false;
          mobile_ephemeral_last_start_error_.clear();
          log().warning << "Mobile ephemeral listen stopped";
        });
      };
      if (node_runtime_ && node_runtime_->EphemeralListenActive()) {
        node_runtime_->StopEphemeralListenAsync([this, finish = std::move(finish)]() mutable {
          BrowserThread::PostTask(BrowserThreadId::IO, std::move(finish));
        });
        return;
      }
      finish();
    });
    return;
  }

  if (!node_runtime_->EphemeralListenActive()) {
    return;
  }

  const bool want_relay = MobileInCallRelayEligible(call_sessions_.get());
  if (want_relay && media_relay_ && !mobile_ephemeral_relay_started_) {
    media_relay_->Start();
    mobile_ephemeral_relay_started_ = true;
    ApplyMeshAdmissionPolicies();
  } else if (!want_relay && mobile_ephemeral_relay_started_ && media_relay_) {
    media_relay_->Stop();
    mobile_ephemeral_relay_started_ = false;
    ApplyMeshAdmissionPolicies();
  }
}

void MessagingHub::SyncLanMdnsAdvertisement() {
  if (!lan_mdns_ || !lan_mdns_->IsRunning() || !node_runtime_ || !node_runtime_->Host()) {
    return;
  }

  const Libp2pRole role = ResolveLibp2pRole(config_.libp2p);
  const bool node_listen = role == Libp2pRole::Node && node_runtime_->IsRunning();
  const bool ephemeral = node_runtime_->EphemeralListenActive();
  if (!node_listen && !ephemeral) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  std::string peer_id;
  if (auto local = node_runtime_->Host()->LocalPeerIdBase58()) {
    peer_id = *local;
  }
  const std::string bound = node_runtime_->BoundListenMultiaddr();
  const auto port = TcpPortFromMultiaddr(bound);
  if (peer_id.empty() || !port || *port <= 0) {
    lan_mdns_->SetAdvertisement({}, 0, {});
    return;
  }

  std::vector<std::string> ips;
  if (ephemeral) {
    ips = BuildMobileCallScopedAdvertisedAddrs(bound, peer_id);
  } else {
    ips = BuildMobileCallScopedAdvertisedAddrs(bound, peer_id);
  }
  std::vector<std::string> host_ips;
  for (const std::string& ma : ips) {
    const std::string ip = IpHostFromMultiaddrPrefix(ma);
    if (!ip.empty()) {
      host_ips.push_back(ip);
    }
  }
  lan_mdns_->SetAdvertisement(peer_id, *port, host_ips);
}

void MessagingHub::OnLanMdnsPeerDiscovered(const LanMdnsDiscoveredPeer& peer) {
  // mDNS thread — hop to Browser IO before touching sessions / host repos.
  BrowserThread::PostTask(BrowserThreadId::IO, [this, peer]() {
    if (peer.peer_id_base58.empty()) {
      return;
    }
    if (lan_mdns_contact_peer_ids_.find(peer.peer_id_base58) == lan_mdns_contact_peer_ids_.end()) {
      return;
    }
    auto ma = LanMdnsDiscovery::BuildMultiaddr(peer);
    if (!ma) {
      return;
    }
    PeerSessionManager* sessions = Sessions();
    if (sessions == nullptr) {
      return;
    }
    log().warning << "LAN mDNS discovered peer=" << peer.peer_id_base58 << " ma=" << *ma;
    (void)sessions->UpsertBookEntry(peer.peer_id_base58, *ma, PeerAddrSource::Mdns);
    (void)sessions->RegisterEndpoint(peer.peer_id_base58, *ma);
    sessions->ClearDialBackoff(peer.peer_id_base58);
    if (p2p_) {
      p2p_->RegisterPeerDirectEndpoint(peer.peer_id_base58, *ma);
    }
    // Call-media dials peer_key=relay:… while mDNS only knows the libp2p PeerId. Alias on IO
    // here — do not wait for UI RegisterContactEndpoints (moto dogfood: undialable despite LAN ma).
    if (contacts_) {
      if (auto listed = contacts_->List(); listed) {
        for (const Contact& contact : *listed) {
          if (PeerIdFromContact(contact) != peer.peer_id_base58) {
            continue;
          }
          const DirectChatTarget target =
              DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
          if (target.peer_identity_value.empty() ||
              target.peer_identity_value == peer.peer_id_base58) {
            continue;
          }
          (void)sessions->RegisterEndpoint(target.peer_identity_value, *ma);
          sessions->ClearDialBackoff(target.peer_identity_value);
          if (p2p_) {
            p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, *ma);
          }
          log().warning << "LAN mDNS dial alias peer=" << peer.peer_id_base58
                        << " dial_key=" << target.peer_identity_value;
        }
      }
    }
    // Do not RegisterContactEndpoints on every mDNS hit — that re-enters sessions/UI and can
    // pile work in front of AcceptInvite. Alias above is enough for call-media dial keys.
  });
}

void MessagingHub::WireCallMediaRelayDeps() {
  if (!call_sessions_) {
    return;
  }
  media_relay_client_ = std::make_unique<MediaRelayServiceClient>(media_relay_.get());
  PeerSessionManager* sessions = node_runtime_ ? node_runtime_->Sessions() : nullptr;
  // Keep dial registry + libp2p media bridge stable across N025 listen sync — recreating them
  // mid-call drops pending answerer state and dangling dial pointers.
  if (!dial_registry_) {
    dial_registry_ = std::make_unique<PeerSessionDialRegistry>(sessions);
  } else {
    dial_registry_->SetSessions(sessions);
  }
  if (sessions && config_.libp2p.capabilities.circuit_relay) {
    if (!circuit_hop_reach_) {
      circuit_hop_reach_ = std::make_unique<CircuitHopReachClient>(
          [this](const std::string& hop_peer_id) { return TryEnsureCircuitHopReachable(hop_peer_id); },
          [this](const std::string& peer_key) { return TryEnsureCallMediaReachable(peer_key); });
    }
  } else {
    circuit_hop_reach_.reset();
  }
  CallSessionManager::MediaRelayDeps deps;
  deps.relay = media_relay_client_.get();
  deps.dial = dial_registry_.get();
  deps.circuit_reach = circuit_hop_reach_.get();
  Libp2pConfig libp2p = config_.libp2p;
  NormalizeLibp2pConfig(libp2p);
  deps.bootstrap_peers = libp2p.bootstrap_peers;
  deps.prefer_contacts = libp2p.prefer_contacts_for_routing;
  if (node_runtime_ && !node_runtime_->BoundListenMultiaddr().empty()) {
    deps.local_listen_multiaddr = node_runtime_->BoundListenMultiaddr();
  } else {
    deps.local_listen_multiaddr = libp2p.listen_multiaddr;
  }
  // Wildcard bind does not identify a LAN subnet for link-scope inference (N023 ns1).
  if (deps.local_listen_multiaddr.find("/ip4/0.0.0.0/") != std::string::npos) {
    deps.local_listen_multiaddr.clear();
  }
  call_sessions_->SetMediaRelayDeps(std::move(deps));

  if (call_media_direct_ && dial_registry_) {
    // Rebuild when CallSessionManager was replaced (BuildMessagingStack) — bridge holds a host&
    // into that object. Keep the same bridge across N025 listen sync on the same manager.
    const bool sessions_changed = (libp2p_bridge_bound_sessions_ != call_sessions_.get());
    if (!call_libp2p_bridge_ || sessions_changed) {
      call_libp2p_bridge_ = std::make_unique<CallLibp2pMediaBridge>(
          call_sessions_->AsP2pSignalingHost(), *call_session_store_, *call_media_keys_, *call_media_engine_,
          *call_media_direct_, dial_registry_.get(), circuit_hop_reach_.get());
      call_sessions_->SetLibp2pMediaBridge(call_libp2p_bridge_.get());
      libp2p_bridge_bound_sessions_ = call_sessions_.get();
      EnsureCallLifecycleBound();
      call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
      log().warning << "CallLibp2pMediaBridge bound (sessions_changed=" << (sessions_changed ? 1 : 0) << ")";
    } else {
      call_libp2p_bridge_->SetReachDeps(dial_registry_.get(), circuit_hop_reach_.get());
      if (call_lifecycle_) {
        call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
      }
    }
  } else {
    call_libp2p_bridge_.reset();
    libp2p_bridge_bound_sessions_ = nullptr;
    call_sessions_->SetLibp2pMediaBridge(nullptr);
  }
}

void MessagingHub::ApplyMeshAdmissionPolicies() {
  const bool prefer = config_.libp2p.prefer_contacts_for_routing;
  const bool node = ResolveLibp2pRole(config_.libp2p) == Libp2pRole::Node;
  const bool mobile_ephemeral =
      Platform::IsMobile() && node_runtime_ && node_runtime_->EphemeralListenActive();
  std::unordered_set<std::string> contact_ids;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      for (const std::string& id : ContactPeerIds(*listed)) {
        contact_ids.insert(id);
      }
    }
  }

  MeshReachabilityClass reach_class = MeshReachabilityClass::Unknown;
  switch (reachability_.Snapshot().status) {
  case ReachabilityStatus::Reachable:
    reach_class = MeshReachabilityClass::Reachable;
    break;
  case ReachabilityStatus::OutboundOnly:
    reach_class = MeshReachabilityClass::OutboundOnly;
    break;
  case ReachabilityStatus::Blocked:
    reach_class = MeshReachabilityClass::Blocked;
    break;
  case ReachabilityStatus::Unknown:
  case ReachabilityStatus::Checking:
  default:
    reach_class = MeshReachabilityClass::Unknown;
    break;
  }

  RelayScopeMask serve_mask = ProviderServeScopeMask(reach_class, node);
  // Org-style: if Node with no contacts loaded, do not refuse strangers.
  const bool force_limit_strangers =
      node && (reach_class == MeshReachabilityClass::OutboundOnly ||
               reach_class == MeshReachabilityClass::Blocked);
  const bool limit_strangers =
      (prefer && node && !contact_ids.empty()) || force_limit_strangers;
  if (!limit_strangers && node) {
    serve_mask |= static_cast<RelayScopeMask>(RelayScope::Public);
  }

  if (circuit_relay_) {
    CircuitRelayAdmissionPolicy policy;
    policy.prefer_contacts_only = limit_strangers;
    policy.serve_scope_mask = serve_mask;
    policy.contact_peer_ids = contact_ids;
    circuit_relay_->SetAdmissionPolicy(std::move(policy));
  }
  if (media_relay_) {
    MediaRelayAdmissionPolicy policy;
    if (mobile_ephemeral) {
      policy.prefer_contacts_only = true;
      policy.serve_scope_mask = kRelayScopeLinkSiteSocial;
      policy.contact_peer_ids = contact_ids;
    } else {
      policy.prefer_contacts_only = limit_strangers;
      policy.serve_scope_mask = serve_mask;
      policy.contact_peer_ids = contact_ids;
    }
    media_relay_->SetAdmissionPolicy(std::move(policy));
  }
}

void MessagingHub::StopLibp2p() {
  mobile_ephemeral_relay_started_ = false;
  mobile_ephemeral_start_inflight_ = false;
  mobile_ephemeral_start_inflight_at_ms_ = 0;
  mobile_ephemeral_stop_inflight_ = false;
  mobile_ephemeral_last_start_error_.clear();
  ephemeral_listen_desired_ = false;
  if (call_lifecycle_) {
    call_lifecycle_->ClearBinding();
  }
  if (call_sessions_) {
    call_sessions_->SetLibp2pMediaBridge(nullptr);
    call_sessions_->SetMediaRelayDeps({});
  }
  call_libp2p_bridge_.reset();
  libp2p_bridge_bound_sessions_ = nullptr;
  if (lan_mdns_) {
    lan_mdns_->Stop();
    lan_mdns_.reset();
  }
  if (call_media_direct_) {
    call_media_direct_->Stop();
    call_media_direct_.reset();
  }
  media_relay_client_.reset();
  dial_registry_.reset();
  if (media_relay_) {
    media_relay_->Stop();
    media_relay_.reset();
  }
  if (circuit_relay_) {
    circuit_relay_->Stop();
    circuit_relay_.reset();
  }
  if (dial_back_) {
    dial_back_->Stop();
    dial_back_.reset();
  }
  if (node_runtime_) {
    node_runtime_->Stop();
    node_runtime_.reset();
  }
}

void MessagingHub::RegisterContactEndpoints() {
  if (!p2p_ || !contacts_) {
    return;
  }
  auto listed = contacts_->List();
  if (!listed) {
    return;
  }
  PeerSessionManager* sessions = Sessions();
  lan_mdns_contact_peer_ids_.clear();
  for (const Contact& contact : *listed) {
    p2p_->RegisterContactDirectEndpoints(contact);
    const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
    if (target.peer_identity_value.empty()) {
      continue;
    }
    for (const std::string& ma : contact.multiaddrs) {
      p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, ma);
    }
    const std::string peer_id = PeerIdFromContact(contact);
    if (!peer_id.empty()) {
      lan_mdns_contact_peer_ids_.insert(peer_id);
    }
    if (peer_id.empty() || sessions == nullptr) {
      continue;
    }
    if (auto ma = sessions->PreferredPeerMultiaddr(peer_id)) {
      (void)sessions->RegisterEndpoint(peer_id, *ma);
      sessions->ClearDialBackoff(peer_id);
      p2p_->RegisterPeerDirectEndpoint(peer_id, *ma);
      if (target.peer_identity_value != peer_id) {
        (void)sessions->RegisterEndpoint(target.peer_identity_value, *ma);
        sessions->ClearDialBackoff(target.peer_identity_value);
        p2p_->RegisterPeerDirectEndpoint(target.peer_identity_value, *ma);
      }
    }
  }
  ApplyMeshAdmissionPolicies();
}

bool MessagingHub::IsContactReachable(const Contact& contact) const {
  return IsContactReachableForMessaging(contact, Sessions(), relay_ != nullptr);
}

void MessagingHub::PrefetchPeerReachability(const std::string& identity) {
  if (!messaging_ready_ || identity.empty() || !node_runtime_) {
    return;
  }
  PeerSessionManager* sessions = node_runtime_->Sessions();
  if (sessions == nullptr) {
    return;
  }

  std::string peer_id;
  if (contacts_) {
    if (auto hit = contacts_->FindByIdentity(identity, ContactIdKind::RelayUser)) {
      if (hit->has_value()) {
        peer_id = PeerIdFromContact(**hit);
      }
    }
    if (peer_id.empty()) {
      if (auto hit = contacts_->FindByIdentity(identity, ContactIdKind::PeerId)) {
        if (hit->has_value()) {
          peer_id = PeerIdFromContact(**hit);
        }
      }
    }
  }
  if (peer_id.empty()) {
    peer_id = identity;
  }

  (void)sessions->ResolvePeerInfo(peer_id);
  if (auto ma = sessions->PreferredPeerMultiaddr(peer_id)) {
    (void)sessions->RegisterEndpoint(peer_id, *ma);
    sessions->ClearDialBackoff(peer_id);
    // Same alias as mDNS: call-media / Chat control use relay identity as dial key.
    if (!identity.empty() && identity != peer_id) {
      (void)sessions->RegisterEndpoint(identity, *ma);
      sessions->ClearDialBackoff(identity);
    }
  }
  // Register endpoints only — do NOT EnsureConnection here. Prefetch runs on CallInvite /
  // CallAccept for both peers; opposite host.connect races the answerer-only call-media dial
  // (dogfood: tablet→phone TCP ESTABLISHED with unread Recv-Q, no Noise/inbound hello).
  // Circuit hops stay on the call-media Connect path (EnsurePeerReachableOnIo).
}

Roe<void> MessagingHub::Initialize(const AppConfig& config, const std::string& profile_data_dir) {
  if (initialized_) {
    return {};
  }

  config_ = config;
  data_dir_ = profile_data_dir;
  std::error_code ec;
  std::filesystem::create_directories(data_dir_, ec);

  profile_id_ = std::filesystem::path(data_dir_).filename().string();
  if (profile_id_.empty()) {
    profile_id_ = "default";
  }

  store_ = std::make_unique<SqliteThreadStore>(data_dir_);
  contacts_ = std::make_unique<ContactsStore>(data_dir_);
  identity_ = std::make_unique<IdentityStore>(data_dir_, profile_id_);

  {
    StartupPhase phase("MessagingHub::ReconcileOutbox");
    (void)store_->ReconcileOutbox();
  }

  InstallServiceClients(config);

  psk_store_ = std::make_unique<SqlitePskSessionStore>(store_->ProfileDbPath(), profile_id_);
  group_roster_ = std::make_unique<GroupRosterStore>(store_->ProfileDbPath());
  call_session_store_ = std::make_unique<CallSessionStore>(store_->ProfileDbPath());
  call_media_keys_ = std::make_unique<CallMediaKeyStore>(store_->ProfileDbPath(), profile_id_);
  call_media_engine_ = std::make_unique<CallMediaEngine>();
  group_invite_gate_ = std::make_unique<GroupInviteGate>(*contacts_, *group_roster_);
  directory_shadows_ = std::make_unique<DirectoryShadowCache>(*directory_);
  peer_labels_ = std::make_unique<PeerDisplayResolver>(*contacts_, *directory_shadows_, group_roster_.get());
  inbox_ = std::make_unique<InboxController>(*store_, *contacts_, *peer_labels_, directory_shadows_.get());
  directory_shadows_->SetOnUpdated([this]() {
    if (inbox_) {
      inbox_->NotifyThreadChanged();
    }
  });

  ProfileSecretsService& secrets = ProfileSecretsService::Instance();
  secrets.RegisterDekConsumer(identity_.get());
  secrets.RegisterDekConsumer(psk_store_.get());
  secrets.RegisterDekConsumer(call_media_keys_.get());
  signing_resolver_ = std::make_unique<RelayDirectorySigningKeyResolver>(signing_key_store_, *directory_);
  kem_resolver_ = std::make_unique<RelayDirectoryKemKeyResolver>(kem_key_store_, *directory_);

  // P2P stack without libp2p until profile unlock (relay-capable once identity exists).
  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get(), nullptr, nullptr);
  p2p_->SetProfileDataDir(data_dir_);
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  call_sessions_->SetOnRingChangedMesh([this]() {
    EnsureCallLifecycleBound();
    // Invite ingest runs on IO — never Sync N025 on the IO thread itself.
    if (BrowserThread::CurrentlyOn(BrowserThreadId::UI)) {
      SyncMobileEphemeralListen();
    } else {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { SyncMobileEphemeralListen(); });
    }
  });
  call_sessions_->SetPrefetchPeerReachability([this](const std::string& identity) {
    // Warm only; must not run RequestBridge on Browser IO ahead of AcceptInvite.
    BrowserThread::PostTask(BrowserThreadId::IO, [this, identity]() { PrefetchPeerReachability(identity); });
  });
  EnsureCallLifecycleBound();
  WireCallMediaRelayDeps();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, p2p_.get());

  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    group_invite_gate_->SetInboundPolicy(GroupInvitePolicyFromString(prefs->group_invite_policy));
    if (group_membership_) {
      group_membership_->SetInboundPolicy(GroupInvitePolicyFromString(prefs->group_invite_policy));
    }
  }

  messaging_ready_ = false;
  initialized_ = true;
  return {};
}

void MessagingHub::SetOnMessagingReady(std::function<void()> callback) {
  on_messaging_ready_ = std::move(callback);
}

void MessagingHub::NotifyMessagingReady() {
  if (!on_messaging_ready_) {
    return;
  }
  // EnsureMessagingReady may run on the IO thread (PIN unlock); UI bindings must run on UI.
  if (BrowserThread::CurrentlyOn(BrowserThreadId::UI)) {
    on_messaging_ready_();
    return;
  }
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    if (on_messaging_ready_) {
      on_messaging_ready_();
    }
  });
}

Roe<void> MessagingHub::BuildMessagingStack() {
  WireRelayAuthSigner();

  if (auto libp2p = StartLibp2p(config_); !libp2p) {
    log().warning << "libp2p host start failed: " << libp2p.error().message
                  << " (direct P2P unavailable; relay messaging may still work)";
  }

  p2p_ = std::make_unique<P2pMessagingService>(*store_, *contacts_, *identity_, relay_, *inbox_,
                                                signing_key_store_, *signing_resolver_, kem_key_store_, *kem_resolver_,
                                                *psk_store_, *group_roster_, group_invite_gate_.get(), Libp2p(),
                                                Sessions());
  p2p_->SetProfileDataDir(data_dir_);
  group_membership_ = std::make_unique<GroupMembershipService>(*store_, *contacts_, *identity_, *group_roster_,
                                                               *group_invite_gate_, *p2p_);
  inbox_->SetGroupMembership(group_membership_.get());
  p2p_->SetGroupMembership(group_membership_.get());
  call_sessions_ = std::make_unique<CallSessionManager>(*store_, *contacts_, *identity_, *call_session_store_,
                                                       *call_media_keys_, *p2p_, *psk_store_, *call_media_engine_);
  p2p_->SetCallSessionManager(call_sessions_.get());
  call_sessions_->AbandonOrphanedCallsAfterRestart();
  call_sessions_->SetOnRingChangedMesh([this]() {
    EnsureCallLifecycleBound();
    if (BrowserThread::CurrentlyOn(BrowserThreadId::UI)) {
      SyncMobileEphemeralListen();
    } else {
      BrowserThread::PostTask(BrowserThreadId::UI, [this]() { SyncMobileEphemeralListen(); });
    }
  });
  call_sessions_->SetPrefetchPeerReachability([this](const std::string& identity) {
    BrowserThread::PostTask(BrowserThreadId::IO, [this, identity]() { PrefetchPeerReachability(identity); });
  });
  EnsureCallLifecycleBound();
  WireCallMediaRelayDeps();
  actions_ = std::make_unique<ContactActionDispatcher>(*inbox_, *contacts_, *identity_, *store_,
                                                       group_membership_.get(), registration_, p2p_.get());
  if (auto prefs = UserPreferences::LoadProfile(data_dir_); prefs) {
    const GroupInvitePolicy policy = GroupInvitePolicyFromString(prefs->group_invite_policy);
    group_invite_gate_->SetInboundPolicy(policy);
    group_membership_->SetInboundPolicy(policy);
  }
  RegisterContactEndpoints();
  if (agent_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, *agent_, *store_);
  }
  return {};
}

Roe<void> MessagingHub::EnsureMessagingReady() {
  StartupPhase phase("MessagingHub::EnsureMessagingReady");
  if (!initialized_) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (messaging_ready_) {
    return {};
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    return AppError::Pin(Err::Pin::Required, "Profile vault is locked");
  }
  if (auto identity = identity_->LoadOrCreate(); !identity) {
    return identity.error();
  } else {
    (void)identity;
  }

  if (auto built = BuildMessagingStack(); !built) {
    return built.error();
  }

  messaging_ready_ = true;
  NotifyMessagingReady();
  return {};
}

Roe<void> MessagingHub::Reinitialize(const AppConfig& config, const std::string& profile_data_dir) {
  if (!initialized_) {
    return Initialize(config, profile_data_dir);
  }

  config_ = config;
  UpdateServiceClients(config);
  if (p2p_) {
    p2p_->SetRelayClient(relay_);
  }
  if (actions_) {
    actions_->SetRegistrationClient(registration_);
  }
  if (PeerSessionManager* sessions = Sessions()) {
    sessions->SetConfig(SessionConfigFromApp(config));
  }
  return {};
}

void MessagingHub::BindAgent(AgentSession& agent) {
  agent_ = &agent;
  if (p2p_) {
    router_ = std::make_unique<MessageRouter>(*inbox_, *p2p_, agent, *store_);
  }
}

void MessagingHub::BindSessionStore(SessionStore& store) {
  session_store_ = &store;
}

void MessagingHub::SetWorkerPool(WorkerPool& workers) {
  worker_pool_ = &workers;
}

PeerSigningKeyStore& MessagingHub::SigningKeys() {
  return signing_key_store_;
}

void MessagingHub::TickLibp2p() {
  if (!messaging_ready_) {
    return;
  }
  if (node_runtime_) {
    node_runtime_->Tick();
  }
  if (p2p_) {
    p2p_->TickLibp2p();
  }
  SyncMobileEphemeralListen();
  SyncLanMdnsAdvertisement();
  TickReachabilityUx();
}

ReachabilitySnapshot MessagingHub::Reachability() const {
  return reachability_.Snapshot();
}

void MessagingHub::SetOnReachabilityUpdated(std::function<void()> callback) {
  on_reachability_updated_ = std::move(callback);
}

void MessagingHub::RunReachabilityProbe(bool try_upnp) {
  if (!node_runtime_ || !dial_back_) {
    return;
  }
  reachability_.StartProbe(*node_runtime_, *dial_back_, try_upnp);
}

void MessagingHub::TryUpnpPortMapping() {
  if (!node_runtime_) {
    return;
  }
  const std::string bound = node_runtime_->BoundListenMultiaddr();
  if (ShouldSkipUpnpForListen(bound)) {
    return;
  }
  const auto port = TcpPortFromMultiaddr(bound);
  if (!port) {
    return;
  }
  (void)TryUpnpTcpPortMapping(*port);
  RunReachabilityProbe(false);
}

ProfileIdentityView MessagingHub::LoadProfileIdentityView() {
  ProfileIdentityView view;
  if (!IsInitialized() || !IsMessagingReady()) {
    return view;
  }
  auto identity = Identity().Get();
  if (!identity) {
    return view;
  }
  view.ready = true;
  view.nickname = identity->nickname;
  view.peer_id = identity->peer_id;
  view.relay_id = identity->relay_user_id;
  view.public_key_b64 = identity->public_key_b64;
  FillRegistrationFields(view, *identity);
  return view;
}

Roe<void> MessagingHub::SaveProfileNickname(const std::string& nickname) {
  if (!IsInitialized()) {
    return {};
  }
  auto identity = Identity().Get();
  if (!identity) {
    if (!IsMessagingReady()) {
      return {};
    }
    return identity.error();
  }
  if (nickname == identity->nickname) {
    return {};
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to save identity");
  }

  LocalIdentity updated = *identity;
  updated.nickname = nickname;
  if (auto saved = Identity().Update(updated); !saved) {
    return saved.error();
  }

  if (identity->registered) {
    auto result = UpdateRegisteredNickname(Registration(), Identity(), nickname);
    if (!result) {
      return result.error();
    }
  }
  return {};
}

Roe<void> MessagingHub::RegisterIdentity(const std::string& nickname) {
  if (!IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to register");
  }

  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }

  if (!nickname.empty()) {
    LocalIdentity updated = *identity;
    updated.nickname = nickname;
    if (auto saved = Identity().Update(updated); !saved) {
      return saved.error();
    }
    identity = Identity().Get();
    if (!identity) {
      return identity.error();
    }
  }

  auto applied = FinishAndPersistRegistration(
      Registration(), Identity(), identity->nickname,
      Libp2p() ? Libp2p()->ListenMultiaddrs() : std::vector<std::string>{});
  if (!applied) {
    return applied.error();
  }
  return {};
}

Roe<void> MessagingHub::RotateBriefLlmKey() {
  if (!IsInitialized()) {
    return AppError::Pin(Err::Pin::Required, "Messaging hub not initialized");
  }
  if (!IsMessagingReady()) {
    return AppError::Pin(Err::Pin::Required, "Unlock profile PIN to rotate API key");
  }

  auto identity = Identity().Get();
  if (!identity) {
    return identity.error();
  }
  if (identity->brief_llm_api_key.empty()) {
    return AppError::Auth(Err::Auth::NotRegistered, "No Brief API key yet")
        .WithUser("No Brief API key yet — register on the network in Me first.");
  }

  const AppConfig& config =
      (session_store_ && session_store_->IsInitialized()) ? session_store_->Snapshot().config : config_;
  std::string base_url = config.llm.base_url;
  if (ResolvePreset(config) != "brief" || base_url.empty()) {
    base_url = "https://www.brief.global/api/llm/v1";
  }
  while (!base_url.empty() && base_url.back() == '/') {
    base_url.pop_back();
  }
  const std::string url = base_url + "/keys/rotate";

  auto response = HttpClient::Post(url, "{}", {{"Authorization", "Bearer " + identity->brief_llm_api_key},
                                               {"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response->status_code == 401 || response->status_code == 403) {
    LocalIdentity expired = *identity;
    MarkRegistrationExpired(expired);
    (void)Identity().Update(expired);
    const char* renew_hint = response->status_code == 403
                                 ? "Registration expired — use Renew registration in Me → Profile."
                                 : "Brief API key rejected — renew registration in Me → Profile.";
    return AppError::Auth(Err::Auth::Forbidden,
                          "Brief key rotate HTTP " + std::to_string(response->status_code))
        .WithUser(renew_hint);
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    auto json = nlohmann::json::parse(response->body, nullptr, false);
    std::string detail = "Brief API key rotate failed (HTTP " + std::to_string(response->status_code) + ")";
    if (!json.is_discarded() && json.contains("error")) {
      const auto& err = json["error"];
      if (err.is_string()) {
        detail = err.get<std::string>();
      } else if (err.is_object() && err.contains("message") && err["message"].is_string()) {
        detail = err["message"].get<std::string>();
      }
    }
    return AppError::Network(Err::Network::HttpError, detail);
  }

  auto root = nlohmann::json::parse(response->body, nullptr, false);
  if (root.is_discarded() || !root.contains("llm_api_key") || !root["llm_api_key"].is_string()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate response missing llm_api_key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }
  const std::string new_key = root["llm_api_key"].get<std::string>();
  if (new_key.empty()) {
    return AppError::Auth(Err::Auth::Generic, "Brief API key rotate returned empty key")
        .WithUser("Couldn't update Brief API key — try Renew registration in Me → Profile.");
  }

  LocalIdentity updated = *identity;
  updated.brief_llm_api_key = new_key;
  if (auto saved = Identity().Update(updated); !saved) {
    return saved.error();
  }
  return {};
}

void MessagingHub::TickReachabilityUx() {
  if (!config_.libp2p.node_enabled || Platform::IsMobile()) {
    return;
  }
  const ReachabilitySnapshot snap = reachability_.Snapshot();
  if (snap.status != ReachabilityStatus::OutboundOnly && snap.status != ReachabilityStatus::Blocked) {
    reachability_outbound_since_ms_ = 0;
    return;
  }
  const uint64_t now = SDL_GetTicks();
  if (reachability_outbound_since_ms_ == 0) {
    reachability_outbound_since_ms_ = now;
    return;
  }
  if (reachability_banner_shown_ || now - reachability_outbound_since_ms_ < 30000) {
    return;
  }
  reachability_banner_shown_ = true;
  if (on_reachability_updated_) {
    on_reachability_updated_();
  }
}

void MessagingHub::RefreshMeshCapabilities() {
  if (!messaging_ready_ || !node_runtime_) {
    return;
  }
  if (session_store_ && session_store_->IsInitialized()) {
    config_.libp2p = session_store_->Snapshot().config.libp2p;
  }
  if (circuit_relay_) {
    circuit_relay_->Stop();
    circuit_relay_.reset();
  }
  if (media_relay_) {
    media_relay_->Stop();
    media_relay_.reset();
  }
  mobile_ephemeral_relay_started_ = false;
  media_relay_client_.reset();
  dial_registry_.reset();
  const Libp2pRole role = ResolveLibp2pRole(config_.libp2p);
  circuit_relay_ = std::make_unique<CircuitRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.circuit_relay) {
    circuit_relay_->Start();
  }
  media_relay_ = std::make_unique<MediaRelayService>(*node_runtime_->Host(), *node_runtime_->Sessions());
  media_relay_->SetBudget(config_.libp2p.media_relay_budget);
  media_relay_->SetPricing(config_.libp2p.pricing.media_relay);
  if (role == Libp2pRole::Node && config_.libp2p.capabilities.media_relay) {
    media_relay_->Start();
  }
  ApplyMeshAdmissionPolicies();
  WireCallMediaRelayDeps();
  SyncMobileEphemeralListen();
}

void MessagingHub::Apply(const NetworkConfig& next) {
  if (!initialized_) {
    return;
  }

  const bool service_urls_changed = next.relay.base_url != config_.relay.base_url ||
                                    next.directory.base_url != config_.directory.base_url ||
                                    next.registration.base_url != config_.registration.base_url;
  const bool mesh_changed =
      next.node_enabled != config_.libp2p.node_enabled ||
      next.circuit_relay != config_.libp2p.capabilities.circuit_relay ||
      next.media_relay != config_.libp2p.capabilities.media_relay ||
      next.prefer_contacts_for_routing != config_.libp2p.prefer_contacts_for_routing;

  config_.relay = next.relay;
  config_.directory = next.directory;
  config_.registration = next.registration;
  config_.libp2p.node_enabled = next.node_enabled;
  config_.libp2p.capabilities.circuit_relay = next.circuit_relay;
  config_.libp2p.capabilities.media_relay = next.media_relay;
  config_.libp2p.prefer_contacts_for_routing = next.prefer_contacts_for_routing;

  if (service_urls_changed) {
    UpdateServiceClients(config_);
    if (p2p_) {
      p2p_->SetRelayClient(relay_);
    }
    if (actions_) {
      actions_->SetRegistrationClient(registration_);
    }
  }
  if (PeerSessionManager* sessions = Sessions()) {
    sessions->SetConfig(SessionConfigFromApp(config_));
  }
  if (mesh_changed && messaging_ready_) {
    RefreshMeshCapabilities();
  }
}

void MessagingHub::Apply(const PolicyPrefs& prefs) {
  if (!initialized_) {
    return;
  }
  if (group_invite_gate_) {
    group_invite_gate_->SetInboundPolicy(prefs.group_invite_policy);
  }
  if (group_membership_) {
    group_membership_->SetInboundPolicy(prefs.group_invite_policy);
  }
}

void MessagingHub::Apply(const NotificationPrefs& prefs) {
  if (!initialized_ || !messaging_ready_) {
    return;
  }
  (void)PushDeviceCoordinator::SyncWithPreference(*this, prefs.show_notifications);
}

MessagingHub::NetworkConfig MessagingHub::ProjectNetwork(const AppConfig& config) {
  NetworkConfig out;
  out.relay = config.relay;
  out.directory = config.directory;
  out.registration = config.registration;
  out.node_enabled = config.libp2p.node_enabled;
  out.circuit_relay = config.libp2p.capabilities.circuit_relay;
  out.media_relay = config.libp2p.capabilities.media_relay;
  out.prefer_contacts_for_routing = config.libp2p.prefer_contacts_for_routing;
  return out;
}

MessagingHub::PolicyPrefs MessagingHub::ProjectPolicy(const ProfilePreferences& prefs) {
  return {.group_invite_policy = GroupInvitePolicyFromString(prefs.group_invite_policy)};
}

MessagingHub::NotificationPrefs MessagingHub::ProjectNotifications(const ProfilePreferences& prefs) {
  return {.show_notifications = prefs.show_notifications};
}

Roe<CircuitRelayBridgeResult> MessagingHub::RequestCircuitBridgePreferred(const std::string& target_peer_id,
                                                                          const std::string& target_multiaddr,
                                                                          int timeout_ms) {
  if (!circuit_relay_ || !node_runtime_ || !node_runtime_->Sessions()) {
    return Error("circuit-relay not available");
  }
  if (target_peer_id.empty() && target_multiaddr.empty()) {
    return Error("missing circuit bridge target");
  }
  std::vector<Contact> contacts;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      contacts = std::move(*listed);
    }
  }
  Libp2pConfig libp2p = config_.libp2p;
  NormalizeLibp2pConfig(libp2p);
  auto hops = OrderCircuitHops(CollectContactHopCandidates(contacts), CollectSeedHopCandidates(libp2p.bootstrap_peers),
                               libp2p.prefer_contacts_for_routing);
  if (hops.empty()) {
    return Error("no circuit hop candidates");
  }

  CircuitBridgeTarget target;
  target.target_peer_id = target_peer_id;
  target.target_multiaddr = target_multiaddr;

  CircuitRelayBridgeResult last;
  last.error = "all hops failed";
  PeerSessionManager& sessions = *node_runtime_->Sessions();
  for (const MeshHopCandidate& hop : hops) {
    const std::string key = hop.peer_id;
    if (!target.target_peer_id.empty() && key == target.target_peer_id) {
      continue;
    }
    if (hop.multiaddr.empty()) {
      if (auto ma = sessions.PreferredPeerMultiaddr(key)) {
        (void)sessions.RegisterEndpoint(key, *ma);
      }
    } else {
      (void)sessions.RegisterEndpoint(key, hop.multiaddr);
    }
    sessions.ClearDialBackoff(key);
    if (!sessions.IsDialable(key)) {
      last.error = "hop not dialable: " + key;
      continue;
    }
    auto bridged = circuit_relay_->RequestBridge(key, target, timeout_ms);
    if (!bridged) {
      last.error = bridged.error().message;
      continue;
    }
    if (bridged->ok) {
      return *bridged;
    }
    last = *bridged;
  }
  return last;
}

std::vector<std::string> MessagingHub::CollectDialableCircuitRelayIds(const std::string& exclude_peer_id) const {
  std::vector<std::string> relay_ids;
  if (!node_runtime_ || !node_runtime_->Sessions()) {
    return relay_ids;
  }
  PeerSessionManager& sessions = *node_runtime_->Sessions();
  std::vector<Contact> contacts;
  if (contacts_) {
    if (auto listed = contacts_->List()) {
      contacts = std::move(*listed);
    }
  }
  Libp2pConfig libp2p = config_.libp2p;
  NormalizeLibp2pConfig(libp2p);
  auto hops = OrderCircuitHops(CollectContactHopCandidates(contacts), CollectSeedHopCandidates(libp2p.bootstrap_peers),
                               libp2p.prefer_contacts_for_routing);
  relay_ids.reserve(hops.size());
  for (const MeshHopCandidate& hop : hops) {
    if (hop.peer_id.empty() || hop.peer_id == exclude_peer_id) {
      continue;
    }
    if (hop.multiaddr.empty()) {
      if (auto ma = sessions.PreferredPeerMultiaddr(hop.peer_id)) {
        (void)sessions.RegisterEndpoint(hop.peer_id, *ma);
      }
    } else {
      (void)sessions.RegisterEndpoint(hop.peer_id, hop.multiaddr);
    }
    sessions.ClearDialBackoff(hop.peer_id);
    if (sessions.IsDialable(hop.peer_id)) {
      relay_ids.push_back(hop.peer_id);
    }
  }
  return relay_ids;
}

Roe<void> MessagingHub::TryEnsureCircuitHopReachable(const std::string& hop_peer_id) {
  if (!circuit_relay_ || !node_runtime_ || !node_runtime_->Sessions()) {
    return Error("circuit-relay not available");
  }
  if (!config_.libp2p.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  PeerSessionManager& sessions = *node_runtime_->Sessions();
  const std::vector<std::string> relay_ids = CollectDialableCircuitRelayIds(hop_peer_id);
  if (relay_ids.empty()) {
    return Error("no dialable circuit relays");
  }
  return sessions.TryEnsureHopViaCircuit(hop_peer_id, *circuit_relay_, relay_ids, kMediaRelayProtocolId, 8000);
}

Roe<void> MessagingHub::TryEnsureCallMediaReachable(const std::string& peer_key) {
  if (!circuit_relay_ || !node_runtime_ || !node_runtime_->Sessions()) {
    return Error("circuit-relay not available");
  }
  if (!config_.libp2p.capabilities.circuit_relay) {
    return Error("circuit-relay disabled");
  }
  if (peer_key.empty()) {
    return Error("missing call peer");
  }

  PeerSessionManager& sessions = *node_runtime_->Sessions();
  std::string target = peer_key;
  if (contacts_) {
    if (auto hit = contacts_->FindByIdentity(peer_key, ContactIdKind::RelayUser)) {
      if (hit->has_value()) {
        const std::string peer_id = PeerIdFromContact(**hit);
        if (!peer_id.empty()) {
          target = peer_id;
        }
      }
    }
    if (target == peer_key) {
      if (auto hit = contacts_->FindByIdentity(peer_key, ContactIdKind::PeerId)) {
        if (hit->has_value()) {
          const std::string peer_id = PeerIdFromContact(**hit);
          if (!peer_id.empty()) {
            target = peer_id;
          }
        }
      }
    }
  }

  if (sessions.IsDialable(peer_key) || sessions.IsDialable(target)) {
    if (target != peer_key) {
      if (auto ma = sessions.PreferredPeerMultiaddr(target)) {
        (void)sessions.RegisterEndpoint(peer_key, *ma);
      }
    }
    return {};
  }

  const std::vector<std::string> relay_ids = CollectDialableCircuitRelayIds(target);
  if (relay_ids.empty()) {
    return Error("no dialable circuit relays");
  }
  auto reached =
      sessions.TryEnsureHopViaCircuit(target, *circuit_relay_, relay_ids, kCallMediaDirectProtocolId, 8000);
  if (reached && target != peer_key) {
    if (auto ma = sessions.PreferredPeerMultiaddr(target)) {
      (void)sessions.RegisterEndpoint(peer_key, *ma);
    }
  }
  return reached;
}

void MessagingHub::SuspendLibp2pColdPeers() {
  SyncMobileEphemeralListen();
  if (node_runtime_) {
    node_runtime_->SuspendColdPeers();
  }
}

void MessagingHub::Shutdown() {
  if (!initialized_) {
    return;
  }

  on_messaging_ready_ = nullptr;
  on_reachability_updated_ = nullptr;
  agent_ = nullptr;

  if (router_) {
    router_->SetOnLocalAction(nullptr);
    router_->SetSharedAiConfirmCallback(nullptr);
  }
  if (actions_) {
    actions_->SetOnActionMessage(nullptr);
  }
  if (inbox_) {
    inbox_->SetOnThreadChanged(nullptr);
  }
  if (p2p_) {
    p2p_->SetOnMessagesChanged(nullptr);
    p2p_->SetOnDeliveryNotice(nullptr);
    p2p_->SetOnBackgroundUnread(nullptr);
    p2p_->SetGroupMembership(nullptr);
  }
  if (inbox_) {
    inbox_->SetGroupMembership(nullptr);
  }
  if (call_sessions_) {
    call_sessions_->ClearMediaCallbacks();
  }

  router_.reset();
  store_->Flush();
  contacts_->Flush();
  if (messaging_ready_) {
    identity_->Flush();
  }
  actions_.reset();
  call_sessions_.reset();
  // Destroy P2P before groups — P2P held a non-owning Groups pointer.
  p2p_.reset();
  group_membership_.reset();
  group_invite_gate_.reset();
  group_roster_.reset();
  StopLibp2p();
  signing_resolver_.reset();
  kem_resolver_.reset();
  ProfileSecretsService& secrets = ProfileSecretsService::Instance();
  if (identity_) {
    secrets.UnregisterDekConsumer(identity_.get());
  }
  if (psk_store_) {
    secrets.UnregisterDekConsumer(psk_store_.get());
  }
  if (call_media_keys_) {
    secrets.UnregisterDekConsumer(call_media_keys_.get());
  }
  call_media_engine_.reset();
  call_media_keys_.reset();
  call_session_store_.reset();
  psk_store_.reset();
  inbox_.reset();
  peer_labels_.reset();
  directory_shadows_.reset();
  http_relay_.reset();
  http_directory_.reset();
  http_registration_.reset();
  http_push_devices_.reset();
  http_client_compat_.reset();
  relay_ = nullptr;
  push_devices_ = nullptr;
  client_compat_ = nullptr;
  directory_ = nullptr;
  registration_ = nullptr;
  http_relay_url_.clear();
  http_directory_url_.clear();
  http_registration_url_.clear();
  identity_.reset();
  contacts_.reset();
  store_.reset();
  signing_key_store_.Clear();
  kem_key_store_.Clear();
  messaging_ready_ = false;
  initialized_ = false;
}

InboxController& MessagingHub::Inbox() {
  return *inbox_;
}

P2pMessagingService& MessagingHub::P2p() {
  return *p2p_;
}

GroupMembershipService& MessagingHub::Groups() {
  return *group_membership_;
}

CallSessionManager* MessagingHub::Calls() {
  return call_sessions_.get();
}

CallLifecycle* MessagingHub::Lifecycle() {
  EnsureCallLifecycleBound();
  return call_lifecycle_.get();
}

void MessagingHub::EnsureCallLifecycleBound() {
  if (!call_sessions_) {
    if (call_lifecycle_) {
      call_lifecycle_->ClearBinding();
    }
    return;
  }
  if (!call_lifecycle_) {
    call_lifecycle_ = std::make_unique<CallLifecycle>();
  }
  call_lifecycle_->Bind(call_sessions_.get());
  call_lifecycle_->SetOnListenDesireChanged([this](bool want) { SetEphemeralListenDesire(want); });
  if (call_libp2p_bridge_) {
    call_libp2p_bridge_->SetLifecycle(call_lifecycle_.get());
  }
}

void MessagingHub::SetEphemeralListenDesire(bool want) {
  ephemeral_listen_desired_ = want;
  SyncMobileEphemeralListen();
}

MessageRouter& MessagingHub::Router() {
  return *router_;
}

ContactActionDispatcher& MessagingHub::Actions() {
  return *actions_;
}

IThreadStore& MessagingHub::Store() {
  return *store_;
}

ContactsStore& MessagingHub::Contacts() {
  return *contacts_;
}

IdentityStore& MessagingHub::Identity() {
  return *identity_;
}

IDirectoryClient& MessagingHub::Directory() {
  return *directory_;
}

DirectoryShadowCache& MessagingHub::DirectoryShadows() {
  return *directory_shadows_;
}

PeerDisplayResolver& MessagingHub::PeerLabels() {
  return *peer_labels_;
}

IRegistrationClient& MessagingHub::Registration() {
  return *registration_;
}

IPushDeviceClient* MessagingHub::PushDevices() {
  return push_devices_;
}

IClientCompatClient* MessagingHub::ClientCompat() {
  return client_compat_;
}

Libp2pHost* MessagingHub::Libp2p() {
  return node_runtime_ ? node_runtime_->Host() : nullptr;
}

PeerSessionManager* MessagingHub::Sessions() const {
  return node_runtime_ ? node_runtime_->Sessions() : nullptr;
}

} // namespace pbr
