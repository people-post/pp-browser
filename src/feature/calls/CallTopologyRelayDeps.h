#pragma once

#include "mesh/host/MeshPorts.h"
#include "mesh/l4/circuit/client/AmpCircuitHopRegistry.h"
#include "mesh/reach/MeshReachPorts.h"
#include "feature/call_media/ICallMediaTransport.h"
#include "mesh/l4/media_relay/client/IMediaRelayClient.h"
#include "mesh/media_plane/MediaRelayAttach.h"
#include "mesh/l4/media_relay/MediaRelayTypes.h"
#include "common/media/CallMediaHealth.h"
#include "amp/link/Types.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * SoftMigrate / hop pick wiring (mesh clients + hop discovery).
 * Owned by CallTopologyController; CallHopMigrateWorkflow holds a non-owning pointer (V047).
 */
struct CallTopologyMediaRelayDeps {
  mesh::IMediaRelayClient* relay = nullptr;
  mesh::IDialRegistry* dial = nullptr;
  mesh::ICircuitHopReach* circuit_reach = nullptr;
  /** Mesh media plane liveness for the three pointers above (null = ungated; tests). */
  pp::Lifetime::Token objects_alive;
  uint64_t objects_snap = 0;
  /** The mesh's `work` executor: where an attach continues off the IO strand (null = inline; tests). */
  std::shared_ptr<pp::Executor> work;
  std::vector<std::string> bootstrap_peers;
  bool prefer_contacts = true;
  /** projects/privacy T4: only org seeds and Friendly contacts' nodes carry call media. */
  bool trusted_relays_only = false;
  /** Cached mesh_node listings (n-dir). */
  std::function<std::vector<mesh::MeshDirectoryNode>()> list_directory_nodes;
  /** DHT peer_routing cache (n2-caps). */
  std::function<std::vector<mesh::MeshDirectoryNode>()> list_dht_nodes;
  /** When false, org seed hops are omitted (bridge score / n-dir). */
  std::function<bool()> seed_dial_ok;
  /** The mesh's reach record holds this peer down (it could not be reached lately): not a hop now. */
  std::function<bool(const std::string& peer_id)> reach_held_down;
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

  /** Attach / quote ports carrying the plane's liveness gate. */
  mesh::MediaRelayAttachPorts AttachPorts() const {
    mesh::MediaRelayAttachPorts ports;
    ports.relay = relay;
    ports.dial = dial;
    ports.service_reach = circuit_reach;
    ports.objects_alive = objects_alive;
    ports.objects_snap = objects_snap;
    ports.work = work;
    return ports;
  }
};

} // namespace pbr
