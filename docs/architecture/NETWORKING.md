# Networking doctrine

**Tier:** architecture  
**Status:** Product north star (2026-07-31)

pp-browser uses **HTTP** plus a **peer mesh**. Product peer mesh migrates to **AMP** (UDP + Session/Channel) under [`projects/adp`](../../projects/adp/); vendored libp2p shrinks toward PeerId/crypto helpers ([A017](../../projects/adp/DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)). Agents must not introduce a third realtime stack (e.g. WebRTC/libdatachannel) as the product path.

## Planes

| Plane | Preferred role |
|-------|----------------|
| **HTTP** | Contact **org backends** when reachable: Brief APIs, relay store/history, client-compat, quotes/billing UX, settlement orchestration |
| **Peer mesh (AMP)** | **Peer data exchange** when `MeshHost` Amp is up: messaging, call signaling, **call media**, mesh capabilities (`media_relay`, circuit, …). Libp2p TCP/Identify only as soft-fail fallback if Amp bind fails |

## Settlement (pricing)

| Path | When |
|------|------|
| **HTTP backend** | **Preferred** when available — receipts, rate cards, user-facing bills |
| **Direct chain settle** | **Backup** when HTTP is not an option (or policy prefers trust-minimized pay) |

Meters and hop **quotes** still originate on the mesh ([p2p-mesh](../../projects/p2p-mesh/) N019–N020). Backend/chain are how those obligations **settle**, not a second media path.

## Libp2p / Amp investment

**AMP** ([`projects/adp`](../../projects/adp/)) is the product underlay when `libp2p.enable_amp_stack` succeeds: UDP + MSH + channels; ch0 replaces Identify; Amp accept is independent of TCP listen. D9 step 6: MeshHost skips Identify + TCP mesh listen + DialBack/libp2p relay hosting when Amp owns the mesh.

The **vendored** fork under [`src/lib/libp2p/`](../../src/lib/libp2p/) remains for PeerId crypto and transitional fallback. Mesh work continues to deepen:

- Peer discovery and dialability (ch0 / mDNS `amp_udp`, contacts, bootstrap; Identify retired on Amp path)
- Reachability (Amp UDP accept, circuit nested Session, SoftMigrate)
- Routing and transmission (L3 channels, FRAG/QoS, budgets)
- Price incentives (quotes, ceilings, volunteer → paid regulation)

**Hop reachability** continues in-mesh ([media-hop-reachability](../../projects/media-hop-reachability/)). SoftMigrate must not grow a parallel NAT toolkit.

**Ownership planes:** Profile (app/node-local secrets + identity) → **MeshHost** (shared) → **MessagingHub** / MessagingCore + **CallStack** (app-only) → **MessagingFacade** / CallUiBackend (UI).

`MeshHost` ([`src/base/p2p/`](../../src/base/p2p/)) is the **shared mesh composition root** — Amp stack + L4 coordinators when enabled; otherwise `NodeRuntime` + dial-back + circuit/media relay + reachability — converging the `MessagingHub` and headless `pp-node` start paths.

See [p2p-mesh](../../projects/p2p-mesh/) (N022+) and [adp](../../projects/adp/).

## Calls

Call **media** product path is **AMP** when Amp is up (voice-first): direct PeerLink channels and/or circuit nested Session + SoftMigrate `media_relay`. Libp2p call-media remains only if Amp fails to start. Wire-compat `call_sdp` / `call_ice` controls are ignored inbound; product does not send them. Code map: [CALLS.md](CALLS.md) · Amp: [CALL_MEDIA_CIRCUIT.md](../../projects/adp/CALL_MEDIA_CIRCUIT.md).

## Related

- [ARCHITECTURE.md](ARCHITECTURE.md) — overall shape  
- [P2P_MESSAGING.md](P2P_MESSAGING.md) — messaging  
- [LIBP2P_STREAMS.md](LIBP2P_STREAMS.md) — stream framing, exchanges, size/hang handling  
- [LIBP2P_UPSTREAM.md](LIBP2P_UPSTREAM.md) — fork deltas  
- [COMPATIBILITY.md](../contracts/COMPATIBILITY.md) — wire/compat  
