#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/mesh/l4/circuit/AmpCircuitHopRegistry.h"
#include "domain/mesh/reachability/MeshReachPorts.h"
#include "domain/mesh/l4/call_media/ICallMediaTransport.h"
#include "domain/mesh/l4/media_relay/IMediaRelayClient.h"
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
