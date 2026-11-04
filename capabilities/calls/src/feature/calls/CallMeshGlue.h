#pragma once

#include "lib/calls/CallTypes.h"
#include "mesh/types/AddressDisclosure.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * What calls asks of this node's mesh, and what it tells it back — handed over once by the stack when
 * it builds the calls (the published mesh view, never the MeshHost). An empty member: that ask has no
 * answer here (harness), the tell goes nowhere. Calls owner.
 */
struct CallMeshGlue {
  // --- asks
  /** Local `/ip4/…/tcp/…/p2p/…` listen set for call-control dial bootstrap. */
  std::function<std::vector<std::string>()> local_listen_multiaddrs;
  /** Local mesh PeerId (base58) for invite / accept — PeerId → relay without contacts. */
  std::function<std::string()> local_mesh_peer_id;
  /** Local capability ads for invite / accept (V030). */
  std::function<CallPeerCaps()> local_peer_caps;
  /** H013: the relays this node is parked on now (a callee advertises them in its CallAccept). */
  std::function<std::vector<std::string>()> parked_relays;
  /** projects/privacy T1: who may learn our listen addresses (and gets a signalling punch answer). Null = anyone. */
  const mesh::AddressDisclosureGate* address_disclosure = nullptr;

  // --- tells
  /** Make this peer reachable by circuit (reserve on the bootstrap seeds). */
  std::function<void()> ensure_circuit_ready;
  /** Async circuit park for the accept gate; `done` comes back as an event. */
  std::function<void(int timeout_ms, std::function<void(bool ready)> done)> park_circuit;
  /** Warm a peer's reachability (async association / DHT lookup). */
  std::function<void(const std::string& identity)> prefetch_peer_reach;
  /** Register a peer's listen multiaddrs (from invite / accept) into the dial registry. */
  std::function<void(const std::string& identity, const std::vector<std::string>& multiaddrs)>
      register_peer_listen_multiaddrs;
  /** The remote's caps for a call — from invite / accept, then `call_caps_update` (K005). */
  std::function<void(const std::string& call_id, const CallPeerCaps& caps)> on_peer_caps;
  /** H013: the relays the call's answerer said it is parked on (its CallAccept). */
  std::function<void(const std::string& call_id, const std::vector<std::string>& relays)> on_peer_parked_relays;
  /** A call learned that `account` speaks from device `peer_id`: its card, asked of that device, confirms it. */
  std::function<void(const std::string& account, const std::string& peer_id)> note_peer_device;
};

} // namespace pbr
