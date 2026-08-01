# Media hop reachability — phases

Implement **in libp2p** ([H001](DECISIONS.md#h001--separate-project-implementation-in-libp2p)). SoftMigrate consume comes last.

## L0 — Docs + ownership

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] H007 — no app `call_hop_addrs` product path; remove uncommitted prototype
- [x] Cross-link NETWORKING / N022 / V026 / CALLS

## L1 — Peer address book in stack

- [x] Host/peerstore: remember multiaddrs per PeerId (Identify, successful dial, bootstrap)
- [x] TTL / replace stale; expose to `PeerSessionManager` / dial helpers
- [x] Document in [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md)

## L2 — Advertise Reachable listen set

- [x] Unify listen + UPnP external + DialBack-observed into advertised Identify addrs
- [x] Node + `media_relay` on ⇒ hop candidates get fresh ads when peers connect

## L3 — Circuit PeerId-friendly dial

- [x] Evolve custom circuit toward dial-by-PeerId when relay already has target (or reservation)
- [x] SoftMigrate may use circuit when direct `IsDialable` fails (H005)
- **Gap:** L3 is **single-hop only** (one relay must direct-dial target). Multi-hop plan: [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md) / [H008](DECISIONS.md#h008--multi-hop-circuit-chains-planned).

## L3.5 — Multi-hop circuit v2

Docs: [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md), [H008](DECISIONS.md#h008--multi-hop-circuit-chains-planned), mesh [N024](../p2p-mesh/DECISIONS.md#n024--circuit-pricing-pay-immediate-relay-only). **Not started.**

- [ ] ADR + spec (H008, N024, MULTI_HOP_CIRCUIT) — **done**
- [ ] Protocol v2: `bridge_path`, nested `sub_bridge`, max 2 relays, loop detection
- [ ] R1 upstream relay selection (provider inner loop); consumer still picks R1 only
- [ ] Session model: opaque end-to-end tunnel handle (replace single `CircuitHopLink` assumption)
- [ ] Integrate with ns3 bridge score (“R1 can reach B” incl. subcontract)
- [ ] Tests + [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md) fork notes

## L4 — SoftMigrate consume stack only

- [ ] Rank hops as today; skip if stack says undialable
- [ ] Drop reliance on empty contact ma as the only signal (contacts optional cache — H003)
- [ ] Dogfood: Android SoftMigrate → Windows Node without pasted multiaddr

## L5 — Directory / DHT (later)

- [ ] Per N015 timing; still closed-set for media hops (N020)
