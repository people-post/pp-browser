# Mesh / L4 / P2P organization proposal (post-AMP extraction)

**Status:** Discussion draft  
**Context:** AMP L1–L3 + link moved to `pp-cpp-amp` (PR #142). Product mesh is Amp-only (D10). Vendored libp2p is PeerId/crypto only (A017).

## Problem statement

After AMP extraction, mesh-related code is **architecturally unified** but **physically scattered** and **naming-stale**:

| Symptom | Example |
|---------|---------|
| Legacy names imply libp2p Host | `StartLibp2p()`, `Libp2pHostConfig`, `P2pMessagingService`, `CallLibp2pMediaBridge` |
| L4 split across layers | Coordinators in `base/p2p/`; chat/call consumers in `feature/messaging/` |
| Reachability spread | `ReachabilityService`, `NatTraversal`, `LanMdnsDiscovery`, `AmpDialBackService`, network-status-chrome |
| Policy vs transport mixed | `MeshHopPolicy` (people), `SoftMigrateLogic` (messaging), relay types (p2p) |
| Docs predate cutover | `p2p-mesh/CURRENT_STATE.md`, `media-hop-reachability/*`, `SRC_LAYOUT.md` still reference deleted types |

The stack is no longer "libp2p + glue." It is:

```
pp-cpp-amp (L1 ADP → L2 Session → L3 Channel → link/AmpStack)
     ↑
pp-browser identity (shrunk libp2p PeerId)
     ↑
base/p2p (MeshHost + L4 coordinators + reachability)
     ↑
feature/messaging (L4 chat services + call bridge + hub orchestration)
```

## What is already cohesive (keep)

1. **pp-cpp-amp** — transport stack; no product protocols; acyclic L1→L2→L3→link.
2. **MeshHost** — single composition root for Amp + L4 coordinators + reachability.
3. **L4 coordinator pattern** — pure `*BundleLogic` + io-thread `*Coordinator` on `MeshRuntime`.
4. **Contracts** — `AMP-CHANNEL.md`, `WIRE_SCHEMAS.md`, `NETWORKING.md` (D10 doctrine).

## Proposed organization (three axes)

### Axis 1: Rename "mesh" as the product concept

Stop using "libp2p" / "p2p" in **runtime** names. Reserve libp2p for the **PeerId fork** only.

| Current | Proposed | Notes |
|---------|----------|-------|
| `src/base/p2p/` | `src/base/mesh/` | Directory rename (optional phase 2) |
| `Libp2pHostConfig` | `MeshIdentityConfig` | ML-DSA keys → Amp PeerId |
| `StartLibp2p()` | `StartMesh()` | MessagingHub entry |
| `P2pMessagingService` | `MeshMessagingService` | Feature layer |
| `CallLibp2pMediaBridge` | `CallMediaBridge` | Amp-only now |
| `libp2p.mesh_enabled` config key | keep for compat; alias `mesh.enabled` in docs | Config migration later |

**Keep:** `src/lib/libp2p/` as-is (fork name is accurate).

### Axis 2: Subdivide `base/mesh/` (or `base/p2p/`) by concern

Flat ~60 files is navigable but mixes roles. Suggested subdirs (no layer violation):

```
base/mesh/
  host/           MeshHost, MeshHostConfig (identity + start/stop)
  identity/       PeerIdUtil  (links p2p_peer_id target)
  reachability/   Reachability*, NatTraversal, LanMdns*, AmpDialBackService
  l4/
    circuit/      CircuitTunnelCoordinator, CircuitBundleLogic, AmpCircuitHopRegistry
    media_relay/  AmpMediaRelayCoordinator, MediaRelay*
    call_media/   CallMediaLegCoordinator, CallMedia*Logic, ICallMediaTransport
  codec/          LengthPrefixedCodec, ProductChannelPolicies
```

**Rule:** `host/` is the only public entry for feature layer. L4 coordinators stay non-blocking on `MeshRuntime` io thread.

### Axis 3: Clarify L4 ownership (base vs feature)

| Layer | Owns | Examples |
|-------|------|----------|
| **base/mesh/l4/** | Protocol coordinators, wire codecs, relay hosting | circuit-relay, media-relay, call-media transport, dial-back |
| **feature/messaging/** | App-facing services, JSON payloads, hub wiring | AmpDirectChat, AmpChatHistory, AmpChatBlob, CallStack, AmpMediaRelayClient |
| **base/people/** | Hop ranking policy (no wire) | MeshHopPolicy |
| **base/messaging/** | SoftMigrate, peer caps | SoftMigrateLogic, PeerCapsLogic |

**Invariant:** feature never opens raw `PeerLink*`; always goes through MeshHost deps or adopted `ChannelSession` via registry.

## Documentation consolidation

Single entry point: **`docs/architecture/MESH.md`** (new) linking:

- Stack: `projects/adp/STACK.md`
- L4 contracts: `docs/contracts/AMP-CHANNEL.md`
- Product status: `projects/p2p-mesh/CURRENT_STATE.md` (refresh)
- Reachability: `projects/media-hop-reachability/`
- Calls: `docs/architecture/CALLS.md`
- Legacy libp2p: `docs/architecture/LIBP2P_UPSTREAM.md` (PeerId fork only)

Retire or add banners to stale docs referencing `PeerSessionManager`, TCP underlay, `NodeRuntime`, in-tree `src/lib/amp/`.

## libp2p fork: finish the shrink

A017 left PeerId + `keys_wire`. Next cleanup (low risk):

1. Move `PeerIdUtil` next to fork or into `pp-cpp-amp` link layer if PeerId becomes AMP-native type.
2. Delete any remaining unlinked Host/TCP sources if still present.
3. Rename CMake target `p2p_peer_id` → `mesh_peer_id` (optional; breaks external refs).

## Suggested phased rollout

| Phase | Work | Risk |
|-------|------|------|
| **P0 — docs** | `MESH.md` index; refresh `CURRENT_STATE.md`; fix `SRC_LAYOUT.md` paths | None |
| **P1 — rename API** | Type aliases + deprecated wrappers (`using Libp2pHostConfig = MeshIdentityConfig`) | Low |
| **P2 — subdirs** | Physical move under `base/mesh/` with CMake target unchanged | Medium (includes) |
| **P3 — config** | `mesh.enabled` alias; deprecate `libp2p.mesh_enabled` | Medium (compat) |

Do **not** block feature work on P2/P3. P0 unblocks onboarding immediately.

## Open questions

1. Should `pp-cpp-amp` expose a single `MeshRuntime` header path so product code never includes `amp/link/*` directly?
2. Is `CallMediaLegCoordinator` correctly base-layer, or should call-media L4 move to `feature/calls/` with a thinner transport port?
3. Headless `pp-node` — should it share `MeshHost` only, or get a `NodeMeshHost` slim variant?
4. When does the shrunk libp2p fork get extracted to `pp-cpp-common` or a tiny `pp-peer-id` repo?

## Decision needed

Approve P0 doc refresh as immediate next step? Rename (`p2p` → `mesh`) as P1, or keep directory name and fix naming in API only?
