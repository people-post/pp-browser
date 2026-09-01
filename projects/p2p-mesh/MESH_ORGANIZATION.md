# Mesh / L4 organization (post-rename)

**Status:** Landed on `cursor/mesh-rename-p2p-to-mesh-69f5`  
**Context:** AMP L1–L3 + link in `pp-cpp-amp`; product mesh is Amp-only (D10).

## What changed

### Directory

- `src/base/p2p/` → **`src/base/mesh/`**
- CMake target `pp_base_p2p` → **`pp_base_mesh`**

### Config (JSON + C++)

| Before | After |
|--------|-------|
| `"libp2p": { ... }` in config.json | `"mesh": { ... }` (primary) |
| `AppConfig.libp2p` | `AppConfig.mesh` |
| `Libp2pConfig` | `MeshConfig` |
| `Libp2pRole` | `MeshRole` |

**Compat:** `ConfigJson` still reads legacy `"libp2p"` key when `"mesh"` is absent.

### API renames

| Before | After |
|--------|-------|
| `Libp2pHostConfig` | `MeshIdentityConfig` |
| `P2pMessagingService` | `MeshMessagingService` |
| `CallLibp2pMediaBridge` | `CallMediaBridge` |
| `MessagingHub::StartLibp2p()` | `StartMesh()` |
| `MessagingHub::P2p()` | `MeshMessaging()` |
| `LastLibp2pError()` | `LastMeshError()` |
| `libp2p_status_message` (UI) | `mesh_status_message` |

### Unchanged (wire / fork)

- Multiaddr `/p2p/<PeerId>` wire format
- Call control JSON field `libp2p_peer_id`
- Vendored fork `src/lib/libp2p/` and CMake targets `p2p_peer_id`, `p2p_wire`

## Layer map

```
pp-cpp-amp (L1–L3 + link)
     ↑
src/lib/libp2p (PeerId only)
     ↑
src/base/mesh (MeshHost + L4 coordinators + reachability)
     ↑
src/feature/messaging (MeshMessagingService, CallMediaBridge, hub)
```

## Future (optional)

- Subdivide `base/mesh/` into `host/`, `reachability/`, `l4/`, `codec/`
- Add `docs/architecture/MESH.md` index doc
- Extract shrunk libp2p fork to tiny `pp-peer-id` repo
