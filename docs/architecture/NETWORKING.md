# Networking doctrine

**Tier:** architecture  
**Status:** Product north star (2026-08-31 — D10 Amp hard-require)

pp-browser uses **HTTP** plus a **peer mesh**. Product peer mesh is **AMP** (UDP + Session/Channel) under [`projects/adp`](../../projects/adp/); vendored libp2p shrinks toward PeerId/crypto helpers ([A017](../../projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)). Agents must not introduce a third realtime stack (e.g. WebRTC/libdatachannel) as the product path.

## Planes

| Plane | Preferred role |
|-------|----------------|
| **HTTP** | Contact **org backends** when reachable: Brief APIs, relay store/history, client-compat, quotes/billing UX, settlement orchestration |
| **Peer mesh (AMP)** | **Peer data exchange** when `mesh.mesh_enabled` (default true): messaging, call signaling, **call media**, mesh capabilities (`media_relay`, circuit, …). Amp bind failure **fails mesh start** (D10 — no TCP underlay fallback). `mesh_enabled=false` leaves peer mesh off |

## Settlement (pricing)

| Path | When |
|------|------|
| **HTTP backend** | **Preferred** when available — receipts, rate cards, user-facing bills |
| **Direct chain settle** | **Backup** when HTTP is not an option (or policy prefers trust-minimized pay) |

Meters and hop **quotes** still originate on the mesh ([p2p-mesh](../../projects/p2p-mesh/) N019–N020). Backend/chain are how those obligations **settle**, not a second media path.

## Amp mesh (D10)

**AMP** ([`projects/adp`](../../projects/adp/)) is the only product underlay when `mesh_enabled` is on: UDP + MSH + channels; ch0 replaces Identify; Amp UDP accept is always on. MeshHost does **not** start TCP listen, Identify, DialBack, or libp2p circuit/media-relay hosting.

**LAN keep:** mDNS TXT `amp_udp=`, PreferLocal / invite Amp multiaddrs (`BuildAmpLanAdvertisedAddrs`), contact/ch0 ADP addrs. **WAN inbound chrome:** Amp dial-back (D8) + optional UPnP UDP; needs ADP bootstrap peers for seed dial.

The **vendored** fork under [`src/lib/libp2p/`](../../src/lib/libp2p/) is still in-tree, but product binaries link **PeerId/crypto helpers only** (`p2p_peer_id`, `p2p_wire` — [A017](../../projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)). Idle Host/stream sources are unlinked pending delete. Mesh work continues to deepen:

- Peer discovery and dialability (ch0 / mDNS `amp_udp`, contacts, bootstrap)
- Reachability (Amp UDP accept, circuit nested Session, SoftMigrate, Amp dial-back)
- Routing and transmission (L3 channels, FRAG/QoS, budgets)
- Price incentives (quotes, ceilings, volunteer → paid regulation)

**Hop reachability** continues in-mesh ([media-hop-reachability](../../projects/media-hop-reachability/)), including planned **Amp Coordinated Punch** ([HOLE_PUNCH.md](../../projects/media-hop-reachability/HOLE_PUNCH.md)). SoftMigrate must not grow a parallel NAT toolkit.

**Ownership planes:** Profile (app/node-local secrets + identity) → **MeshHost** (Amp composition root) → **ConversationsHub** / ConversationsCore + **CallStack** (app-only) → **ConversationsFacade** / CallUiBackend (UI).

See [p2p-mesh](../../projects/p2p-mesh/) (N022+) and [adp](../../projects/adp/).

**L4 protocol kinds:** freeze growth around seven conversation shapes (identify / discover / reach / circuit / rpc / blob / realtime) — [L4_PROTOCOL_KINDS.md](../contracts/L4_PROTOCOL_KINDS.md) ([A028](../../projects/adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes), [N030](../../projects/p2p-mesh/DECISIONS.md#n030--adopt-l4-protocol-kinds-gate)). Do not add a new `protocol_id` per feature.

## Calls

Call **media** product path is **AMP** (voice-first): direct PeerLink channels and/or circuit nested Session + SoftMigrate `media_relay`. Wire-compat `call_sdp` / `call_ice` controls are ignored inbound; product does not send them. Code map: [CALLS.md](CALLS.md) · Amp: [CALL_MEDIA_CIRCUIT.md](../../projects/adp/CALL_MEDIA_CIRCUIT.md).

## Related

- [ARCHITECTURE.md](ARCHITECTURE.md) — overall shape  
- [P2P_MESSAGING.md](P2P_MESSAGING.md) — messaging  
- [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md) — **legacy** stream framing notes (product underlay is Amp)  
- [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md) — fork deltas  
- [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) — wire/compat  
