# Media hop reachability — phases

**[DESIGN.md](DESIGN.md) is the authoritative specification** (stack model, consume API). **This file orders work only** — checklists and traceability. Rationale: [DECISIONS.md](DECISIONS.md). Implementation truth: [CURRENT_STATE.md](CURRENT_STATE.md).

Implement **in Amp mesh** ([H001](DECISIONS.md#h001--separate-project-implementation-in-amp-mesh)). SoftMigrate consume comes last. **Punch (L3.25) and multi-hop (L3.5) are parallel** — neither blocks the other.

## L0 — Docs + ownership

- [x] README / DESIGN / DECISIONS / PHASES / CURRENT_STATE
- [x] H007 — no app `call_hop_addrs` product path; remove uncommitted prototype
- [x] Cross-link NETWORKING / N022 / V026 / CALLS
- [x] Amp framing reset + [HOLE_PUNCH.md](HOLE_PUNCH.md) / H009 (2026-09)

## L1 — Peer address book in stack

- [x] Host address book: remember multiaddrs per PeerId (ch0 / dial success / bootstrap)
- [x] TTL / replace stale; expose to dial helpers / hop reach
- [ ] Refresh Amp-era notes if any Identify-only docs remain

## L2 — Advertise Reachable listen set

- [x] Unify listen + UPnP external + DialBack-observed into advertised Amp addrs
- [x] Node + `media_relay` on ⇒ hop candidates get fresh ads when peers connect

## L3 — Circuit PeerId-friendly dial

- [x] Evolve custom circuit toward dial-by-PeerId when relay already has target (or reservation)
- [x] SoftMigrate may use circuit when direct `IsDialable` fails (H005)
- [x] Loopback compose: circuit + call-media + media_relay fan-out (`loopback_partition_fixture.h`)
- **Gap:** L3 is **single-hop only** (one relay must direct-dial target). Multi-hop plan: [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md) / [H008](DECISIONS.md#h008--multi-hop-circuit-chains-planned).

## L3.25 — Amp Coordinated Punch

Docs: [HOLE_PUNCH.md](HOLE_PUNCH.md), [H009](DECISIONS.md#h009--amp-coordinated-punch-acp), preference [H002](DECISIONS.md#h002--publish-in-stack--punch--circuit--fail). **Parallel** to L3.5.

- [x] ADR + spec (H009, HOLE_PUNCH) — **done**
- [ ] **L3.25a** — Observed/listen/advertise lifecycle on Amp; cold punch via seed introducer; A026 election + A027 teardown
- [ ] **L3.25b** — Contact introducer; address-book upsert on punch success; SoftMigrate dialability benefit
- [ ] **L3.25c** — Upgrade-from-circuit (R1 as introducer); promote direct PeerLink; demote circuit
- [ ] Tests: dual-dial race, sync-window expiry → circuit fallback; no CGNAT overclaim in hard-lab

## L3.5 — Multi-hop circuit v2

Docs: [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md), [H008](DECISIONS.md#h008--multi-hop-circuit-chains-planned), mesh [N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker). **Later** when single-hop cannot reach B (R1↛B, R2 can). **Parallel** to L3.25 punch.

- [x] ADR + spec (H008, N024, MULTI_HOP_CIRCUIT) — **done**
- [ ] Protocol v2: `bridge_path`, nested `sub_bridge`, `circuit_relay.max_hops` (default 3, config-only limit), loop detection
- [ ] R1 upstream relay selection (provider inner loop); consumer still picks R1 only
- [ ] Session model: opaque end-to-end tunnel handle (replace single `CircuitHopLink` assumption)
- [ ] Integrate with ns3 bridge score (“R1 can reach B” incl. subcontract)
- [ ] Tests + Amp/mesh notes

## L4 — SoftMigrate consume stack only

- [x] Rank hops; skip when stack says undialable (after circuit attempt)
- [x] Drop reliance on empty contact ma as the only signal (contacts optional cache — H003)
- [x] Loopback: circuit-backed SoftMigrate-style quote/attach/fan-out (`CircuitMediaRelayComposeTest`)
- [x] Mobile ephemeral listen (N025): addrs published during Wi‑Fi foreground call for PeerId dial
- [x] UI + `RegisterContactEndpoints` consume address book; call-time prefetch via seed/circuit
- [ ] After L3.25: treat punch-success as direct dialable (no SoftMigrate NAT logic)

## L5 — Directory / DHT (later)

- [ ] Per N015 timing; still closed-set for media hops (N020)
