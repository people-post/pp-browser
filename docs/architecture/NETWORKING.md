# Networking doctrine

**Tier:** architecture  
**Status:** Product north star (2026-07-31)

pp-browser uses **two network stacks**. Agents must not introduce a third realtime stack (e.g. WebRTC/libdatachannel) as the product path.

## Planes

| Plane | Preferred role |
|-------|----------------|
| **HTTP** | Contact **org backends** when reachable: Brief APIs, relay store/history, client-compat, quotes/billing UX, settlement orchestration |
| **libp2p** | **Peer data exchange**: messaging, call signaling, **call media**, mesh capabilities (`media_relay`, circuit, …). Also reach backends / mesh peers when HTTP is blocked or unavailable |

## Settlement (pricing)

| Path | When |
|------|------|
| **HTTP backend** | **Preferred** when available — receipts, rate cards, user-facing bills |
| **Direct chain settle** | **Backup** when HTTP is not an option (or policy prefers trust-minimized pay) |

Meters and hop **quotes** still originate on the mesh ([p2p-mesh](../../projects/p2p-mesh/) N019–N020). Backend/chain are how those obligations **settle**, not a second media path.

## Libp2p investment

The **vendored** fork under [`src/libp2p/fork/`](../../src/libp2p/fork/) is a product surface, not a thin dependency. Mesh work continues to deepen:

- Peer discovery and dialability (peerstore / Identify, contacts, bootstrap, later directory/DHT)
- Reachability (listen, UPnP, dial-back, circuit → PeerId-friendly paths)
- Routing and transmission (streams, framing/QoS, budgets)
- Price incentives (quotes, ceilings, volunteer → paid regulation)

**Hop reachability** is implemented **inside** that stack ([media-hop-reachability](../../projects/media-hop-reachability/) — program + consume; [H001](../../projects/media-hop-reachability/DECISIONS.md#h001--separate-project-implementation-in-libp2p) / [H007](../../projects/media-hop-reachability/DECISIONS.md#h007--no-app-layer-hop-candidate-exchange-as-product-path)). SoftMigrate must not grow a parallel NAT toolkit.

**Ownership planes:** Profile (app/node-local secrets + identity) → **MeshHost** (shared) → **MessagingHub** / MessagingCore + **CallStack** (app-only) → **MessagingFacade** / CallUiBackend (UI).

`MeshHost` ([`src/libp2p/integration/host/`](../../src/libp2p/integration/host/)) is the **shared mesh composition root** — `NodeRuntime` + dial-back + circuit/media relay + reachability — converging the `MessagingHub` and headless `pp-node` start paths. Mesh UX reads through `MeshHost` (via `MessagingHub::Mesh()`), not ad-hoc hub forwards.

See [p2p-mesh](../../projects/p2p-mesh/) (N022+).

## Calls

Call **media** product path is **libp2p-only** (voice-first): direct peer streams and/or blind `media_relay`. Wire-compat `call_sdp` / `call_ice` controls are ignored inbound; product does not send them. Product ADR: [V026](../../projects/p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking). Code map: [CALLS.md](CALLS.md).

## Related

- [ARCHITECTURE.md](ARCHITECTURE.md) — overall shape  
- [P2P_MESSAGING.md](P2P_MESSAGING.md) — messaging  
- [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md) — stream framing, exchanges, size/hang handling  
- [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md) — fork deltas  
- [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) — wire/compat  
