#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/media/CallMediaHealth.h"
#include "amp/link/Types.h"

#include "common/Error.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Narrow client surface for CallTopologyController (fakeable in unit tests). */
class IMediaRelayClient {
public:
  virtual ~IMediaRelayClient() = default;

  virtual Roe<std::string> LocalPeerIdBase58() const = 0;
  virtual bool IsStarted() const = 0;
  virtual Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key,
                                            const MediaRelayQuoteRequest& request,
                                            int timeout_ms = 8000) = 0;
  /** Prefer over sync RequestQuote when MeshPump + PostToIo are available. */
  virtual void RequestQuoteAsync(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                 std::function<void(Roe<MediaRelayQuote>)> on_done, int timeout_ms = 8000) {
    if (on_done) {
      on_done(RequestQuote(hop_peer_key, request, timeout_ms));
    }
  }
  virtual Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame,
      int timeout_ms = 8000) = 0;
  virtual void AcceptAndAttachAsync(const std::string& hop_peer_key, const std::string& quote_id,
                                    const std::string& call_id, const std::string& auth_stub,
                                    std::function<void(MediaDataFrame)> on_frame,
                                    std::function<void(Roe<MediaRelayAttachResult>)> on_done,
                                    int timeout_ms = 8000) {
    if (on_done) {
      on_done(AcceptAndAttach(hop_peer_key, quote_id, call_id, auth_stub, std::move(on_frame), timeout_ms));
    }
  }
  /** After AcceptAndAttach + StartSfu — begin inbound frame delivery. */
  virtual void StartClientFrameReader() = 0;
  /**
   * Unexpected guest duplex death (not Detach). Default no-op for fakes that never lose transport.
   * Handler may be invoked on the mesh io thread.
   */
  virtual void SetClientTransportLostHandler(std::function<void()> /*handler*/) {}
  /** In-call hop: join local HostSession without dialing self. */
  virtual Roe<MediaRelayAttachResult> AttachAsLocalHop(
      const std::string& call_id, std::function<void(MediaDataFrame)> on_frame) = 0;
  virtual Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) = 0;
  virtual Roe<void> SendFrame(const MediaDataFrame& frame) = 0;
  virtual void Detach() = 0;
  virtual bool IsAttached() const = 0;
  virtual bool IsLocalHopAttached() const = 0;
  /** Hop drop pressure 0..1 (V032); default 0 for fakes. */
  virtual double PathPressure() const { return 0.0; }
  /** Hop health counters (V032); default empty. */
  virtual CallHopHealth HealthSnapshot() const { return {}; }
};

/** Dial registry surface for hop RegisterEndpoint / IsDialable. */
class IDialRegistry {
public:
  virtual ~IDialRegistry() = default;

  virtual Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) = 0;
  virtual bool IsDialable(const std::string& peer_key) const = 0;
  /** PeerLink Connected — stricter than IsDialable (has_endpoint alone is not enough). */
  virtual bool IsConnected(const std::string& peer_key) const {
    (void)peer_key;
    return false;
  }
  /** Kick ADP dial/handshake; optional for fakes. */
  virtual void EnsureAssociation(const std::string& peer_key,
                                 std::function<void(Roe<void>)> on_done) {
    (void)peer_key;
    if (on_done) {
      on_done(Error("ensure association not available"));
    }
  }
  virtual std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const = 0;
  virtual void ClearDialBackoff(const std::string& peer_key) = 0;
  virtual void AbortInflightDial(const std::string& peer_key) = 0;
  virtual void ClearCallMediaCircuitHop(const std::string& peer_key) = 0;
  /** True when call-media nested circuit carrier hop is installed for peer. */
  virtual bool HasCallMediaCircuitHop(const std::string& peer_key) const {
    (void)peer_key;
    return false;
  }
};

/** L3: circuit bridge fallback when hop PeerId is not directly dialable. */
class ICircuitHopReach {
public:
  virtual ~ICircuitHopReach() = default;
  /** Reach a media_relay hop (topology / prefetch). */
  virtual Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) = 0;
  /** Prefer over sync when MeshPump + PostToIo are available. */
  virtual void TryEnsureHopReachableAsync(const std::string& hop_peer_id,
                                          std::function<void(Roe<void>)> on_done) {
    if (on_done) {
      on_done(TryEnsureHopReachable(hop_peer_id));
    }
  }
  /** Reach a call peer for 1:1 call-media when not directly dialable.
   *  `allow_circuit`: when false, punch only and wait for peer Connected (answerer waits for
   *  offerer circuit dial — reverse-dial needs the offerer parked on the seed). */
  virtual Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) = 0;
  virtual void TryEnsureCallMediaReachableAsync(const std::string& peer_key,
                                                std::function<void(Roe<void>)> on_done,
                                                bool allow_circuit = true) {
    (void)allow_circuit;
    if (on_done) {
      on_done(TryEnsureCallMediaReachable(peer_key));
    }
  }
  /** L3.25c: punch via circuit R1 as introducer, then demote the circuit hop. */
  virtual Roe<void> TryUpgradeToDirect(const std::string& peer_key) {
    (void)peer_key;
    return Error("circuit upgrade not available");
  }
  virtual void TryUpgradeToDirectAsync(const std::string& peer_key,
                                       std::function<void(Roe<void>)> on_done) {
    if (on_done) {
      on_done(TryUpgradeToDirect(peer_key));
    }
  }
  /** Abort in-flight EnsureViaCircuit / punch chains (ConnectFailed / Leave / teardown). */
  virtual void AbortPending() {}
};

/** Amp-only dial registry (PeerLinkManager + AmpCircuitHopRegistry). */
class PeerSessionDialRegistry final : public IDialRegistry {
public:
  PeerSessionDialRegistry() = default;

  void SetAmpLinks(IChatPeerLinks* amp_links) { amp_links_ = amp_links; }
  void SetAmpCircuitHops(AmpCircuitHopRegistry* hops) { amp_hops_ = hops; }
  /** MeshRuntime::PostToIo — EnsureAssociation must run on the Amp IO strand. */
  void SetPostIo(std::function<void(std::function<void()>)> post_io) { post_io_ = std::move(post_io); }

  Roe<void> RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) override {
    if (amp_links_) {
      if (IsAdpMultiaddr(multiaddr)) {
        (void)amp_links_->RegisterEndpoint(peer_key, multiaddr);
        if (auto peer_id = PeerIdFromAdpMultiaddr(multiaddr); peer_id && *peer_id != peer_key) {
          (void)amp_links_->RegisterEndpoint(*peer_id, multiaddr);
        }
        return {};
      }
    }
    return Error("dial registry not available");
  }

  bool IsDialable(const std::string& peer_key) const override {
    if (amp_links_) {
      if (amp_links_->GetLinkSnapshot(peer_key).has_endpoint) {
        return true;
      }
      // L3.25b: successful punch may leave a Connected PeerLink under PeerId before/without
      // a separate endpoint row — SoftMigrate should still treat that as direct-dialable.
      if (amp_links_->IsConnected(peer_key)) {
        return true;
      }
    }
    // SoftMigrate hop dialability is media-relay-specific. Call-media circuit hops
    // must not mark a peer dialable for quote/attach (TryEnsureCallMediaReachable
    // remains the call-media path and is protocol-keyed).
    return amp_hops_ && static_cast<bool>(amp_hops_->Find(peer_key, kMediaRelayProtocolId));
  }

  bool IsConnected(const std::string& peer_key) const override {
    return amp_links_ && amp_links_->IsConnected(peer_key);
  }

  void EnsureAssociation(const std::string& peer_key,
                         std::function<void(Roe<void>)> on_done) override {
    auto run = [this, peer_key, on_done = std::move(on_done)]() mutable {
      if (!amp_links_) {
        if (on_done) {
          on_done(Error("dial registry not available"));
        }
        return;
      }
      amp_links_->EnsureAssociation(peer_key, [on_done = std::move(on_done)](IChatPeerLinks::LinkRoe r) {
        if (!on_done) {
          return;
        }
        if (!r) {
          on_done(Error(r.error().message));
          return;
        }
        on_done({});
      });
    };
    if (post_io_) {
      post_io_(std::move(run));
      return;
    }
    run();
  }

  std::optional<std::string> PreferredMultiaddr(const std::string& peer_key) const override {
    if (amp_links_) {
      if (auto amp_ma = amp_links_->PreferredMultiaddr(peer_key)) {
        return amp_ma;
      }
    }
    return std::nullopt;
  }

  void ClearDialBackoff(const std::string& peer_key) override {
    if (amp_links_) {
      amp_links_->ClearDialBackoff(peer_key);
    }
  }

  void AbortInflightDial(const std::string& peer_key) override {
    if (amp_links_) {
      amp_links_->AbortInflightDial(peer_key);
    }
  }

  void ClearCallMediaCircuitHop(const std::string& peer_key) override {
    if (amp_hops_) {
      amp_hops_->Clear(peer_key, kCallMediaDirectProtocolId);
      amp_hops_->Clear(peer_key, pp::amp::kAmpCircuitCarrierProtocolId);
    }
  }

  bool HasCallMediaCircuitHop(const std::string& peer_key) const override {
    return amp_hops_ &&
           (static_cast<bool>(amp_hops_->Find(peer_key, pp::amp::kAmpCircuitCarrierProtocolId)) ||
            amp_hops_->HasAny(peer_key));
  }

private:
  IChatPeerLinks* amp_links_ = nullptr;
  AmpCircuitHopRegistry* amp_hops_ = nullptr;
  std::function<void(std::function<void()>)> post_io_;
};

/** Forwards to ConversationsHub / CallStack wiring. */
class CircuitHopReachClient final : public ICircuitHopReach {
public:
  CircuitHopReachClient(std::function<Roe<void>(const std::string&)> try_media_hop_reach,
                        std::function<Roe<void>(const std::string&)> try_call_media_reach,
                        std::function<Roe<void>(const std::string&)> try_upgrade = {})
      : try_media_hop_reach_(std::move(try_media_hop_reach)),
        try_call_media_reach_(std::move(try_call_media_reach)),
        try_upgrade_(std::move(try_upgrade)) {}

  Roe<void> TryEnsureHopReachable(const std::string& hop_peer_id) override {
    if (!try_media_hop_reach_) {
      return Error("circuit reach not available");
    }
    return try_media_hop_reach_(hop_peer_id);
  }

  Roe<void> TryEnsureCallMediaReachable(const std::string& peer_key) override {
    if (!try_call_media_reach_) {
      return Error("call-media circuit reach not available");
    }
    return try_call_media_reach_(peer_key);
  }

  Roe<void> TryUpgradeToDirect(const std::string& peer_key) override {
    if (!try_upgrade_) {
      return Error("circuit upgrade not available");
    }
    return try_upgrade_(peer_key);
  }

private:
  std::function<Roe<void>(const std::string&)> try_media_hop_reach_;
  std::function<Roe<void>(const std::string&)> try_call_media_reach_;
  std::function<Roe<void>(const std::string&)> try_upgrade_;
};

/**
 * SoftMigrate / hop pick wiring (mesh clients + hop discovery).
 * Owned by CallTopologyController; CallHopMigrateWorkflow holds a non-owning pointer (V047).
 */
struct CallTopologyMediaRelayDeps {
  IMediaRelayClient* relay = nullptr;
  IDialRegistry* dial = nullptr;
  ICircuitHopReach* circuit_reach = nullptr;
  std::vector<std::string> bootstrap_peers;
  bool prefer_contacts = true;
  /** Cached mesh_node listings (n-dir). */
  std::function<std::vector<MeshDirectoryNode>()> list_directory_nodes;
  /** DHT peer_routing cache (n2-caps). */
  std::function<std::vector<MeshDirectoryNode>()> list_dht_nodes;
  /** When false, org seed hops are omitted (bridge score / n-dir). */
  std::function<bool()> seed_dial_ok;
  /**
   * PreferLocalMediaHop / AttachAsLocalHop — durable Node only (desktop/org).
   * Must stay false for mobile ephemeral media_relay (V027): phones must not SoftMigrate
   * themselves into the SFU host role (dogfood crash + Connection reset for peers).
   */
  bool prefer_local_as_hop = false;
  /** For same-/24 hop ranking only (wildcard bind cleared). */
  std::string local_listen_multiaddr;
  /**
   * Dialable listen multiaddrs for PreferLocalMediaHop CallSfuAttach fan-out
   * (LAN IPs + /p2p/<self>, same shape as call invite listen_multiaddrs).
   * SoftMigrate prefers `resolve_local_advertise` when set (live listen state).
   */
  std::vector<std::string> local_advertise_multiaddrs;
  std::function<std::vector<std::string>()> resolve_local_advertise;
  /**
   * V030: true when peer advertised media_relay on call caps (or equivalent cache).
   * SoftMigrate keeps OrgSeed always; contacts require this. Null → no contact hops.
   */
  std::function<bool(const std::string& peer_id)> peer_has_media_relay;
  /** PeerIds with media_relay=true ads (inject into SoftMigrate when missing from contacts). */
  std::function<std::vector<std::string>()> list_media_relay_peers;
  /**
   * V035: joined remotes’ invite/accept listen multiaddrs (identity → MAs).
   * SoftMigrate InferCallHopScope; missing → Wide.
   */
  std::function<std::unordered_map<std::string, std::vector<std::string>>()>
      resolve_remote_listen_by_peer;
  /**
   * V035: true when peer is LAN-confirmed (mDNS / Amp connected on link).
   * PreferLocal for private advertise requires this — same-/24 alone is insufficient.
   */
  std::function<bool(const std::string& peer_id)> peer_lan_confirmed;
};

} // namespace pbr
