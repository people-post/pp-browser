# Mesh / L4 organization

**Status:** Consolidated on `cursor/mesh-consolidation-69f5`  
**Context:** AMP L1–L3 + link in `pp-cpp-amp`; product mesh is Amp-only (D10).

## Layer map

```
pp-cpp-amp (L1–L3 + link)
     ↑
src/base/mesh/identity (PeerId — native, no libp2p fork)
     ↑
src/base/mesh (host + reachability + l4 coordinators)
     ↑
src/feature/messaging (MeshMessagingService, CallMediaBridge, hub)
```

Subdirectories: `host/`, `identity/`, `reachability/`, `l4/{shared,circuit,media_relay,call_media}/`.

## Identity

- libp2p fork **deleted**; PeerId in `base/mesh/identity/`
- CMake: `pp_base_mesh_identity` (+ `pp_base_mesh`)
- Golden ML-DSA → PeerId tests in `peer_id_util_test.cpp`

## Feature ports

- `MeshHost::ChatDeps()` / `CircuitDeps()` expose `IChatPeerLinks` (adapter over Amp `PeerLinkManager`)
- Feature headers must not include `amp/link/*`

## Config / API (from mesh rename)

| Item | Value |
|------|-------|
| Config JSON key | `"mesh"` (legacy `"libp2p"` still read) |
| Host type | `MeshHost` |
| Messaging service | `MeshMessagingService` |
| Call bridge | `CallMediaBridge` |

## Wire names (unchanged)

- Multiaddr `/p2p/<PeerId>`
- Call JSON `libp2p_peer_id`

## Docs

- [docs/architecture/MESH.md](../../docs/architecture/MESH.md)
- [docs/architecture/MESH_IDENTITY.md](../../docs/architecture/MESH_IDENTITY.md)
