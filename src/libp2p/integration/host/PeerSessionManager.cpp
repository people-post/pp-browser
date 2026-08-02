#include "libp2p/integration/host/PeerSessionManager.h"

#include "libp2p/integration/host/CircuitRelayService.h"
#include "libp2p/integration/host/MediaRelayService.h"

#include <libp2p/host/host.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/network/connection_manager.hpp>
#include <libp2p/network/network.hpp>
#include <libp2p/peer/peer_id.hpp>

#include <algorithm>
#include <system_error>

namespace pbr {

namespace {

Error DialError(const std::string& technical, const std::string& user) {
  return Error(technical).WithUser(user);
}

std::string ProtocolForCircuitLookup(const libp2p::StreamProtocols& protocols) {
  if (protocols.empty()) {
    return {};
  }
  return std::string{protocols.front()};
}

} // namespace

std::string PeerSessionManager::CircuitHopKey(const std::string& peer_key,
                                              const std::string& target_protocol) {
  return peer_key + "\x1f" + target_protocol;
}

std::string PeerDialErrorUserCopy(const std::string& technical_message) {
  if (technical_message.empty()) {
    return {};
  }
  if (technical_message.find("host not running") != std::string::npos) {
    return "Direct messaging is off — check Me → Network.";
  }
  if (technical_message.find("Empty peer endpoint") != std::string::npos ||
      technical_message.find("Invalid multiaddr") != std::string::npos ||
      technical_message.find("missing /p2p/") != std::string::npos ||
      technical_message.find("Invalid PeerId") != std::string::npos) {
    return "Peer address looks wrong — edit the contact multiaddr.";
  }
  if (technical_message.find("not registered") != std::string::npos ||
      technical_message.find("missing PeerInfo") != std::string::npos ||
      technical_message.find("Peer-direct endpoint") != std::string::npos) {
    return "No usable peer address — add a dialable multiaddr on the contact.";
  }
  if (technical_message.find("backoff") != std::string::npos) {
    return "Waiting before retrying the peer connection.";
  }
  if (technical_message.find("Too many concurrent") != std::string::npos) {
    return "Busy connecting to other peers — try again shortly.";
  }
  if (technical_message.find("dial failed") != std::string::npos ||
      technical_message.find("timed out") != std::string::npos ||
      technical_message.find("timeout") != std::string::npos) {
    return "Peer didn't answer — they may be offline or the address may be wrong.";
  }
  if (technical_message.find("stream open failed") != std::string::npos) {
    return "Reached the peer but chat handshake failed.";
  }
  if (technical_message.find("Failed to send") != std::string::npos ||
      technical_message.find("Failed to read") != std::string::npos ||
      technical_message.find("ack") != std::string::npos) {
    return "Direct send didn't confirm — will use relay if available.";
  }
  if (technical_message.find("Relay client not configured") != std::string::npos) {
    return "Couldn't deliver — relay isn't configured.";
  }
  return technical_message;
}

PeerSessionManager::PeerSessionManager(Libp2pHost& host, PeerSessionConfig config)
    : host_(host), config_(std::move(config)), last_sweep_(std::chrono::steady_clock::now()) {
  InstallConnectionHandler();
}

PeerSessionManager::~PeerSessionManager() = default;

PeerAddrSource PeerSessionManager::SourceForEndpointKey(const std::string& peer_relay_user_id) const {
  if (peer_relay_user_id.rfind("bootstrap:", 0) == 0) {
    return PeerAddrSource::Bootstrap;
  }
  if (peer_relay_user_id.rfind("relay:", 0) == 0) {
    return PeerAddrSource::Contact;
  }
  return PeerAddrSource::Manual;
}

void PeerSessionManager::InstallConnectionHandler() {
  if (!host_.IsRunning() || connection_handler_.has_value()) {
    return;
  }
  connection_handler_ = host_.GetHost().setOnNewConnectionHandler(
      [this](libp2p::peer::PeerInfo&& info) { OnInboundConnection(std::move(info)); });
}

void PeerSessionManager::OnInboundConnection(libp2p::peer::PeerInfo info) {
  address_book_.IngestPeerInfo(info, PeerAddrSource::Connection);
  const std::string peer_id = info.id.toBase58();
  std::lock_guard lock(mutex_);
  for (auto& [key, state] : endpoints_) {
    if (state.info && state.info->id == info.id) {
      state.info = info;
      state.last_touch = std::chrono::steady_clock::now();
    }
  }
  MaybeHydrateEndpointFromBookLocked(peer_id);
}

std::optional<std::string> PeerSessionManager::PeerIdBase58ForKeyLocked(
    const std::string& peer_relay_user_id) const {
  const auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end() && it->second.info) {
    return it->second.info->id.toBase58();
  }
  if (libp2p::peer::PeerId::fromBase58(peer_relay_user_id)) {
    return peer_relay_user_id;
  }
  return std::nullopt;
}

void PeerSessionManager::MaybeHydrateEndpointFromBookLocked(const std::string& peer_relay_user_id) {
  auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end() && it->second.info && !it->second.info->addresses.empty()) {
    return;
  }
  const std::optional<std::string> peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id);
  if (!peer_id) {
    return;
  }
  auto ma = address_book_.PreferredMultiaddr(*peer_id);
  if (!ma) {
    return;
  }
  auto parsed = libp2p::multi::Multiaddress::create(*ma);
  if (!parsed) {
    return;
  }
  auto peer = libp2p::peer::PeerId::fromBase58(*peer_id);
  if (!peer) {
    return;
  }
  libp2p::peer::PeerInfo info{peer.value(), {parsed.value()}};
  if (it == endpoints_.end()) {
    EndpointState state;
    state.info = std::move(info);
    state.last_touch = std::chrono::steady_clock::now();
    endpoints_.emplace(peer_relay_user_id, std::move(state));
    return;
  }
  it->second.info = std::move(info);
  it->second.last_touch = std::chrono::steady_clock::now();
}

void PeerSessionManager::SetConfig(PeerSessionConfig config) {
  std::lock_guard lock(mutex_);
  config_ = std::move(config);
}

std::optional<PeerSessionManager::CircuitHopLink> PeerSessionManager::FindCircuitHopLocked(
    const std::string& peer_relay_user_id, const std::string& target_protocol) const {
  if (target_protocol.empty()) {
    return std::nullopt;
  }
  const auto lookup = [&](const std::string& key) -> std::optional<CircuitHopLink> {
    const auto it = circuit_hops_.find(CircuitHopKey(key, target_protocol));
    if (it != circuit_hops_.end()) {
      return it->second;
    }
    return std::nullopt;
  };
  if (auto hit = lookup(peer_relay_user_id)) {
    return hit;
  }
  if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
    if (*peer_id != peer_relay_user_id) {
      return lookup(*peer_id);
    }
  }
  return std::nullopt;
}

void PeerSessionManager::StoreCircuitHopLocked(const std::string& peer_relay_user_id, CircuitHopLink link) {
  if (link.target_protocol.empty()) {
    return;
  }
  circuit_hops_[CircuitHopKey(peer_relay_user_id, link.target_protocol)] = link;
  if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
    if (*peer_id != peer_relay_user_id) {
      circuit_hops_[CircuitHopKey(*peer_id, link.target_protocol)] = link;
    }
  }
}

bool PeerSessionManager::HasAnyCircuitHopLocked(const std::string& peer_relay_user_id) const {
  for (const auto& [key, _] : circuit_hops_) {
    const auto sep = key.find('\x1f');
    if (sep == std::string::npos) {
      continue;
    }
    const std::string stored_peer = key.substr(0, sep);
    if (stored_peer == peer_relay_user_id) {
      return true;
    }
    if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
      if (stored_peer == *peer_id) {
        return true;
      }
    }
  }
  return false;
}

bool PeerSessionManager::HasDirectDialPathLocked(const std::string& peer_relay_user_id) const {
  const auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end()) {
    const auto now = std::chrono::steady_clock::now();
    if (it->second.dial_failed_until > now) {
      return false;
    }
    if (it->second.info && !it->second.info->addresses.empty()) {
      return true;
    }
  }
  if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
    return address_book_.IsDialable(*peer_id);
  }
  return false;
}

Roe<void> PeerSessionManager::RegisterEndpoint(const std::string& peer_relay_user_id,
                                               const std::string& multiaddr) {
  if (peer_relay_user_id.empty() || multiaddr.empty()) {
    return DialError("Empty peer endpoint", "Peer address looks wrong — edit the contact multiaddr.");
  }
  auto parsed = libp2p::multi::Multiaddress::create(multiaddr);
  if (!parsed) {
    return DialError("Invalid multiaddr", "Peer address looks wrong — edit the contact multiaddr.");
  }
  const libp2p::multi::Multiaddress& address = parsed.value();
  const auto peer_id_str = address.getPeerId();
  if (!peer_id_str) {
    return DialError("Multiaddr missing /p2p/ PeerId",
                     "Peer address looks wrong — edit the contact multiaddr.");
  }
  auto peer_id = libp2p::peer::PeerId::fromBase58(*peer_id_str);
  if (!peer_id) {
    return DialError("Invalid PeerId in multiaddr",
                     "Peer address looks wrong — edit the contact multiaddr.");
  }

  libp2p::peer::PeerInfo info{peer_id.value(), {address}};
  if (host_.IsRunning()) {
    (void)host_.GetHost().getPeerRepository().getAddressRepository().upsertAddresses(
        peer_id.value(), std::span<const libp2p::multi::Multiaddress>(info.addresses),
        std::chrono::hours(24));
  }
  (void)address_book_.Upsert(*peer_id_str, multiaddr, SourceForEndpointKey(peer_relay_user_id));

  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    EndpointState state;
    state.info = std::move(info);
    state.last_touch = std::chrono::steady_clock::now();
    endpoints_.emplace(peer_relay_user_id, std::move(state));
  } else {
    const bool was_warm = it->second.warm;
    it->second.info = std::move(info);
    it->second.last_touch = std::chrono::steady_clock::now();
    it->second.warm = was_warm;
  }
  return {};
}

bool PeerSessionManager::IsDialable(const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  if (HasAnyCircuitHopLocked(peer_relay_user_id)) {
    return true;
  }
  return HasDirectDialPathLocked(peer_relay_user_id);
}

bool PeerSessionManager::IsConnected(const std::string& peer_relay_user_id) const {
  std::optional<libp2p::peer::PeerInfo> info;
  {
    std::lock_guard lock(mutex_);
    const auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      return false;
    }
    info = it->second.info;
  }
  if (!host_.IsRunning() || !info) {
    return false;
  }
  return host_.GetHost().connectedness(*info) == libp2p::Host::Connectedness::CONNECTED;
}

bool PeerSessionManager::IsDialing(const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  return inflight_dials_.find(peer_relay_user_id) != inflight_dials_.end();
}

PeerLinkSnapshot PeerSessionManager::GetLinkSnapshot(const std::string& peer_relay_user_id) const {
  PeerLinkSnapshot snap;
  snap.host_running = host_.IsRunning();
  if (!snap.host_running) {
    snap.phase = PeerLinkPhase::Unavailable;
    snap.detail = "libp2p host not running";
    return snap;
  }

  std::optional<libp2p::peer::PeerInfo> info;
  bool dialing = false;
  {
    std::lock_guard lock(mutex_);
    const auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end() || !it->second.info || it->second.info->addresses.empty()) {
      if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
        if (address_book_.IsDialable(*peer_id)) {
          snap.has_endpoint = true;
          snap.phase = PeerLinkPhase::Idle;
          return snap;
        }
      }
      snap.phase = PeerLinkPhase::Unavailable;
      snap.has_endpoint = false;
      if (it != endpoints_.end()) {
        snap.detail = it->second.last_error;
      }
      if (snap.detail.empty()) {
        snap.detail = "Peer endpoint not registered";
      }
      return snap;
    }
    snap.has_endpoint = true;
    snap.detail = it->second.last_error;
    info = it->second.info;
    dialing = inflight_dials_.find(peer_relay_user_id) != inflight_dials_.end();
    const auto now = std::chrono::steady_clock::now();
    if (!dialing && it->second.dial_failed_until > now) {
      snap.phase = PeerLinkPhase::Backoff;
      snap.backoff_remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(it->second.dial_failed_until - now);
      return snap;
    }
  }

  if (dialing) {
    snap.phase = PeerLinkPhase::Dialing;
    return snap;
  }
  if (info && host_.GetHost().connectedness(*info) == libp2p::Host::Connectedness::CONNECTED) {
    snap.phase = PeerLinkPhase::Connected;
    snap.detail.clear();
    return snap;
  }
  snap.phase = PeerLinkPhase::Idle;
  return snap;
}

std::optional<libp2p::peer::PeerInfo> PeerSessionManager::ResolvePeerInfo(
    const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  const auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end() && it->second.info) {
    return it->second.info;
  }
  if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
    return address_book_.ResolvePeerInfo(*peer_id);
  }
  return std::nullopt;
}

std::optional<std::string> PeerSessionManager::PreferredPeerMultiaddr(
    const std::string& peer_relay_user_id) const {
  if (auto info = ResolvePeerInfo(peer_relay_user_id)) {
    if (!info->addresses.empty()) {
      return std::string{info->addresses.front().getStringAddress()};
    }
  }
  if (const auto peer_id = libp2p::peer::PeerId::fromBase58(peer_relay_user_id)) {
    (void)peer_id;
    return address_book_.PreferredMultiaddr(peer_relay_user_id);
  }
  std::lock_guard lock(mutex_);
  if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
    return address_book_.PreferredMultiaddr(*peer_id);
  }
  return std::nullopt;
}

void PeerSessionManager::NoteRemoteIdentify(const std::string& peer_id_base58) {
  if (peer_id_base58.empty()) {
    return;
  }
  address_book_.SyncFromHost(host_, peer_id_base58);
  std::lock_guard lock(mutex_);
  if (auto resolved = address_book_.ResolvePeerInfo(peer_id_base58)) {
    for (auto& [key, state] : endpoints_) {
      if (state.info && state.info->id.toBase58() == peer_id_base58) {
        state.info = resolved;
        state.last_touch = std::chrono::steady_clock::now();
      }
    }
  }
  MaybeHydrateEndpointFromBookLocked(peer_id_base58);
}

Roe<void> PeerSessionManager::UpsertBookEntry(const std::string& peer_id_base58,
                                              const std::string& multiaddr, PeerAddrSource source) {
  return address_book_.Upsert(peer_id_base58, multiaddr, source);
}

Roe<void> PeerSessionManager::TryEnsureHopViaCircuit(const std::string& target_peer_id,
                                                   CircuitRelayService& circuit,
                                                   const std::vector<std::string>& relay_peer_ids,
                                                   const std::string& target_protocol, int timeout_ms) {
  if (target_peer_id.empty()) {
    return Error("missing circuit target peer");
  }
  if (target_protocol.empty()) {
    return Error("missing circuit target protocol");
  }
  {
    std::lock_guard lock(mutex_);
    if (FindCircuitHopLocked(target_peer_id, target_protocol)) {
      return {};
    }
    if (HasDirectDialPathLocked(target_peer_id)) {
      return {};
    }
  }

  CircuitBridgeTarget bridge_target;
  bridge_target.target_peer_id = target_peer_id;
  bridge_target.target_protocol = target_protocol;

  for (const std::string& relay_key : relay_peer_ids) {
    if (relay_key == target_peer_id || !IsDialable(relay_key)) {
      continue;
    }
    auto bridged = circuit.RequestBridge(relay_key, bridge_target, timeout_ms);
    if (!bridged || !bridged->ok || !bridged->stream) {
      continue;
    }
    if (!bridged->resolved_multiaddr.empty()) {
      (void)UpsertBookEntry(target_peer_id, bridged->resolved_multiaddr, PeerAddrSource::AddressRepository);
      (void)RegisterEndpoint(target_peer_id, bridged->resolved_multiaddr);
      ClearDialBackoff(target_peer_id);
    }
    CircuitHopLink link;
    link.stream = bridged->stream;
    link.relay_peer_id = relay_key;
    link.target_protocol = target_protocol;
    {
      std::lock_guard lock(mutex_);
      StoreCircuitHopLocked(target_peer_id, link);
    }
    return {};
  }
  return Error("circuit hop reach failed");
}

bool PeerSessionManager::IsCircuitBacked(const std::string& peer_relay_user_id) const {
  std::lock_guard lock(mutex_);
  return HasAnyCircuitHopLocked(peer_relay_user_id);
}

bool PeerSessionManager::IsCircuitBacked(const std::string& peer_relay_user_id,
                                         const std::string& target_protocol) const {
  std::lock_guard lock(mutex_);
  return FindCircuitHopLocked(peer_relay_user_id, target_protocol).has_value();
}

void PeerSessionManager::ClearCircuitHop(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  for (auto it = circuit_hops_.begin(); it != circuit_hops_.end();) {
    const auto sep = it->first.find('\x1f');
    if (sep == std::string::npos) {
      ++it;
      continue;
    }
    const std::string stored_peer = it->first.substr(0, sep);
    bool matches = stored_peer == peer_relay_user_id;
    if (!matches) {
      if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
        matches = stored_peer == *peer_id;
      }
    }
    if (!matches) {
      ++it;
      continue;
    }
    if (it->second.stream) {
      it->second.stream->close([](auto&&) {});
    }
    it = circuit_hops_.erase(it);
  }
}

void PeerSessionManager::ClearCircuitHop(const std::string& peer_relay_user_id,
                                         const std::string& target_protocol) {
  if (target_protocol.empty()) {
    ClearCircuitHop(peer_relay_user_id);
    return;
  }
  std::vector<std::string> keys;
  keys.push_back(CircuitHopKey(peer_relay_user_id, target_protocol));
  {
    std::lock_guard lock(mutex_);
    if (const auto peer_id = PeerIdBase58ForKeyLocked(peer_relay_user_id)) {
      if (*peer_id != peer_relay_user_id) {
        keys.push_back(CircuitHopKey(*peer_id, target_protocol));
      }
    }
  }
  std::lock_guard lock(mutex_);
  for (const std::string& key : keys) {
    const auto it = circuit_hops_.find(key);
    if (it == circuit_hops_.end()) {
      continue;
    }
    if (it->second.stream) {
      it->second.stream->close([](auto&&) {});
    }
    circuit_hops_.erase(it);
  }
}

void PeerSessionManager::MarkWarm(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return;
  }
  it->second.warm = true;
  it->second.last_touch = std::chrono::steady_clock::now();
}

void PeerSessionManager::ClearWarm(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return;
  }
  it->second.warm = false;
}

void PeerSessionManager::ClearAllWarm() {
  std::lock_guard lock(mutex_);
  for (auto& [_, state] : endpoints_) {
    state.warm = false;
  }
}

void PeerSessionManager::ClearDialBackoff(const std::string& peer_relay_user_id) {
  std::lock_guard lock(mutex_);
  auto it = endpoints_.find(peer_relay_user_id);
  if (it == endpoints_.end()) {
    return;
  }
  it->second.dial_failed_until = {};
  it->second.last_error.clear();
}

void PeerSessionManager::TouchPeerLocked(const std::string& peer_relay_user_id) {
  auto it = endpoints_.find(peer_relay_user_id);
  if (it != endpoints_.end()) {
    it->second.last_touch = std::chrono::steady_clock::now();
  }
}

void PeerSessionManager::DisconnectPeer(const libp2p::peer::PeerId& peer_id) {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this, peer_id]() { host_.GetHost().disconnect(peer_id); });
}

void PeerSessionManager::EvictIfOverCapLocked() {
  if (!host_.IsRunning()) {
    return;
  }
  auto& cmgr = host_.GetHost().getNetwork().getConnectionManager();
  const auto connections = cmgr.getConnections();
  if (connections.size() <= config_.max_connections) {
    return;
  }

  std::vector<std::pair<std::chrono::steady_clock::time_point, std::string>> cold;
  cold.reserve(endpoints_.size());
  for (const auto& [relay_id, state] : endpoints_) {
    if (state.warm) {
      continue;
    }
    cold.emplace_back(state.last_touch, relay_id);
  }
  std::sort(cold.begin(), cold.end());

  size_t to_drop = connections.size() - config_.max_connections;
  for (const auto& [_, relay_id] : cold) {
    if (to_drop == 0) {
      break;
    }
    auto it = endpoints_.find(relay_id);
    if (it == endpoints_.end() || !it->second.info) {
      continue;
    }
    const auto peer_id = it->second.info->id;
    // Disconnect asynchronously; do not nest host ops under assumptions about lock.
    host_.Post([this, peer_id]() { host_.GetHost().disconnect(peer_id); });
    --to_drop;
  }
}

void PeerSessionManager::FinishDial(const std::string& peer_relay_user_id, Roe<void> result) {
  std::vector<DialWaiter> waiters;
  {
    std::lock_guard lock(mutex_);
    auto it = inflight_dials_.find(peer_relay_user_id);
    if (it != inflight_dials_.end()) {
      waiters = std::move(it->second);
      inflight_dials_.erase(it);
    }
    if (!result) {
      auto ep = endpoints_.find(peer_relay_user_id);
      if (ep != endpoints_.end()) {
        ep->second.dial_failed_until = std::chrono::steady_clock::now() + config_.dial_failure_backoff;
        ep->second.last_error = result.error().message;
      }
    } else {
      auto ep = endpoints_.find(peer_relay_user_id);
      if (ep != endpoints_.end()) {
        ep->second.last_error.clear();
        ep->second.dial_failed_until = {};
        if (ep->second.info) {
          const std::string peer_id = ep->second.info->id.toBase58();
          address_book_.SyncFromHost(host_, peer_id);
          if (!ep->second.info->addresses.empty()) {
            (void)address_book_.Upsert(
                peer_id, std::string{ep->second.info->addresses.front().getStringAddress()},
                PeerAddrSource::DialSuccess);
          }
        }
      }
      TouchPeerLocked(peer_relay_user_id);
      EvictIfOverCapLocked();
    }
  }
  if (concurrent_dials_.load() > 0) {
    concurrent_dials_.fetch_sub(1);
  }
  for (auto& waiter : waiters) {
    if (waiter.on_complete) {
      waiter.on_complete(result);
    }
  }
}

void PeerSessionManager::EnsureConnectionOnIo(const std::string& peer_relay_user_id,
                                              std::function<void(Roe<void>)> on_complete) {
  {
    std::lock_guard lock(mutex_);
    if (HasAnyCircuitHopLocked(peer_relay_user_id)) {
      if (on_complete) {
        on_complete({});
      }
      return;
    }
  }

  std::optional<libp2p::peer::PeerInfo> info;
  std::vector<DialWaiter> rejected;
  bool start_dial = false;
  {
    std::lock_guard lock(mutex_);
    MaybeHydrateEndpointFromBookLocked(peer_relay_user_id);
    auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      if (on_complete) {
        on_complete(DialError("Peer endpoint not registered",
                              "No usable peer address — add a dialable multiaddr on the contact."));
      }
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (it->second.dial_failed_until > now) {
      if (on_complete) {
        on_complete(DialError("Peer dial in backoff", "Waiting before retrying the peer connection."));
      }
      return;
    }
    if (!it->second.info) {
      if (on_complete) {
        on_complete(DialError("Peer endpoint missing PeerInfo",
                              "No usable peer address — add a dialable multiaddr on the contact."));
      }
      return;
    }
    info = it->second.info;

    if (host_.GetHost().connectedness(*info) == libp2p::Host::Connectedness::CONNECTED) {
      TouchPeerLocked(peer_relay_user_id);
      it->second.last_error.clear();
      if (on_complete) {
        on_complete({});
      }
      return;
    }

    auto& waiters = inflight_dials_[peer_relay_user_id];
    waiters.push_back(DialWaiter{std::move(on_complete)});
    if (waiters.size() > 1) {
      return;
    }
    if (concurrent_dials_ >= config_.max_concurrent_dials) {
      rejected = std::move(waiters);
      inflight_dials_.erase(peer_relay_user_id);
    } else {
      ++concurrent_dials_;
      start_dial = true;
    }
  }

  if (!rejected.empty()) {
    for (auto& waiter : rejected) {
      if (waiter.on_complete) {
        waiter.on_complete(
            DialError("Too many concurrent dials", "Busy connecting to other peers — try again shortly."));
      }
    }
    return;
  }
  if (!start_dial || !info) {
    return;
  }

  host_.GetHost().connect(*info, [this, peer_relay_user_id](libp2p::Host::ConnectionResult result) {
    if (!result) {
      std::string technical = "libp2p dial failed";
      try {
        technical += ": ";
        technical += result.error().message();
      } catch (...) {
      }
      FinishDial(peer_relay_user_id,
                 DialError(technical,
                           "Peer didn't answer — they may be offline or the address may be wrong."));
      return;
    }
    FinishDial(peer_relay_user_id, {});
  });
}

void PeerSessionManager::EnsureConnection(const std::string& peer_relay_user_id,
                                          std::function<void(Roe<void>)> on_complete) {
  if (!host_.IsRunning()) {
    if (on_complete) {
      on_complete(DialError("libp2p host not running", "Direct messaging is off — check Me → Network."));
    }
    return;
  }
  host_.Post([this, peer_relay_user_id, on_complete = std::move(on_complete)]() mutable {
    EnsureConnectionOnIo(peer_relay_user_id, std::move(on_complete));
  });
}

void PeerSessionManager::OpenStream(const std::string& peer_relay_user_id, libp2p::StreamProtocols protocols,
                                    StreamCb cb) {
  if (!host_.IsRunning()) {
    if (cb) {
      cb(std::make_error_code(std::errc::not_connected));
    }
    return;
  }

  const std::string lookup_protocol = ProtocolForCircuitLookup(protocols);
  {
    std::lock_guard lock(mutex_);
    if (const auto circuit = FindCircuitHopLocked(peer_relay_user_id, lookup_protocol)) {
      if (circuit->stream) {
        libp2p::StreamAndProtocol bridged;
        bridged.stream = circuit->stream;
        if (!protocols.empty()) {
          bridged.protocol = protocols.front();
        }
        TouchPeerLocked(peer_relay_user_id);
        host_.Post([cb = std::move(cb), bridged = std::move(bridged)]() mutable {
          if (cb) {
            cb(bridged);
          }
        });
        return;
      }
    }
  }

  std::optional<libp2p::peer::PeerInfo> info;
  {
    std::lock_guard lock(mutex_);
    MaybeHydrateEndpointFromBookLocked(peer_relay_user_id);
    auto it = endpoints_.find(peer_relay_user_id);
    if (it == endpoints_.end()) {
      if (cb) {
        cb(std::make_error_code(std::errc::no_such_device_or_address));
      }
      return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (it->second.dial_failed_until > now) {
      if (cb) {
        cb(std::make_error_code(std::errc::host_unreachable));
      }
      return;
    }
    if (!it->second.info) {
      if (cb) {
        cb(std::make_error_code(std::errc::no_such_device_or_address));
      }
      return;
    }
    info = it->second.info;
    TouchPeerLocked(peer_relay_user_id);
  }

  if (!info) {
    if (cb) {
      cb(std::make_error_code(std::errc::no_such_device_or_address));
    }
    return;
  }

  host_.Post([this, peer_relay_user_id, info = *info, protocols = std::move(protocols),
              cb = std::move(cb)]() mutable {
    {
      std::lock_guard lock(mutex_);
      EvictIfOverCapLocked();
    }
    host_.GetHost().newStream(info, std::move(protocols),
                              [this, peer_relay_user_id, cb = std::move(cb)](
                                  libp2p::StreamAndProtocolOrError stream_res) mutable {
                                {
                                  std::lock_guard lock(mutex_);
                                  if (!stream_res) {
                                    auto ep = endpoints_.find(peer_relay_user_id);
                                    if (ep != endpoints_.end()) {
                                      ep->second.dial_failed_until =
                                          std::chrono::steady_clock::now() + config_.dial_failure_backoff;
                                      ep->second.last_error = "libp2p chat stream open failed";
                                    }
                                  } else {
                                    auto ep = endpoints_.find(peer_relay_user_id);
                                    if (ep != endpoints_.end()) {
                                      ep->second.last_error.clear();
                                    }
                                    TouchPeerLocked(peer_relay_user_id);
                                  }
                                }
                                if (cb) {
                                  cb(std::move(stream_res));
                                }
                              });
  });
}

void PeerSessionManager::SweepIdle() {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this]() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<libp2p::peer::PeerId> to_disconnect;
    {
      std::lock_guard lock(mutex_);
      for (auto& [relay_id, state] : endpoints_) {
        if (state.warm) {
          continue;
        }
        if (now - state.last_touch < config_.idle_ttl) {
          continue;
        }
        if (!state.info) {
          continue;
        }
        if (host_.GetHost().connectedness(*state.info) == libp2p::Host::Connectedness::CONNECTED) {
          to_disconnect.push_back(state.info->id);
        }
      }
    }
    for (const auto& peer_id : to_disconnect) {
      host_.GetHost().disconnect(peer_id);
    }
  });
}

void PeerSessionManager::SuspendColdPeers() {
  if (!host_.IsRunning()) {
    return;
  }
  host_.Post([this]() {
    std::vector<libp2p::peer::PeerId> to_disconnect;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [_, state] : endpoints_) {
        if (state.warm || !state.info) {
          continue;
        }
        to_disconnect.push_back(state.info->id);
      }
    }
    for (const auto& peer_id : to_disconnect) {
      host_.GetHost().disconnect(peer_id);
    }
  });
}

void PeerSessionManager::Tick() {
  const auto now = std::chrono::steady_clock::now();
  address_book_.PruneExpired();
  if (now - last_sweep_ < std::chrono::seconds(15)) {
    return;
  }
  last_sweep_ = now;
  SweepIdle();
}

size_t PeerSessionManager::RegisteredEndpointCount() const {
  std::lock_guard lock(mutex_);
  return endpoints_.size();
}

size_t PeerSessionManager::WarmPeerCount() const {
  std::lock_guard lock(mutex_);
  size_t count = 0;
  for (const auto& [_, state] : endpoints_) {
    if (state.warm) {
      ++count;
    }
  }
  return count;
}

} // namespace pbr
