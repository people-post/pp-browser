# Media hop reachability — current state

> **2026-09:** Product mesh is Amp-only; hop reachability uses `AmpCircuitHopReach` + `MeshHost::CircuitDeps()`. Hole punch planned as **Amp Coordinated Punch** ([HOLE_PUNCH.md](HOLE_PUNCH.md), H009) — not libp2p DCUtR. See [MESH.md](../../docs/architecture/MESH.md).

**Last updated:** 2026-09-04

## Direction

Hop **reachability** = **Amp mesh stack work** (H001/H007). App-layer `call_hop_addrs` prototype **removed**; do not re-land without ADR. Preference: **publish → punch → circuit → fail** (H002).

**Spec:** [DESIGN.md](DESIGN.md) (concept-first). **Build order:** [PHASES.md](PHASES.md).

## Landed

| Area | State |
|------|-------|
| Project docs | Ownership: Amp mesh implements; SoftMigrate consumes |
| ADRs | H001–H009; circuit multi-hop plan [N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker); punch plan H009 |
| **L1 peer address book** | Stack upsert on bootstrap/register/connect/dial-success; preferred dial addr helpers |
| **L2 advertised listen set** | Amp ch0 + dial-back / UPnP-derived ads |
| **L3 circuit PeerId dial** | Circuit tunnel / hop reach — **single-hop**; SoftMigrate circuit fallback via `ICircuitHopReach` / `AmpCircuitHopReach` |
| **L3 compose (loopback)** | Shared loopback partition fixture; call-media via R; media_relay quote/attach/fan-out via R |

## In progress / gaps

| Area | State |
|------|-------|
| **L3.25 Amp Coordinated Punch** | Spec done — [HOLE_PUNCH.md](HOLE_PUNCH.md); implement L3.25a–c |
| **L3.5 multi-hop circuit** | Spec done — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md); parallel to punch |
| **L4 SoftMigrate consume** | Rank hops; skip undialable after circuit; drop empty contact ma — **loopback compose green**; punch benefit TBD |
| **L5 directory / DHT** | Planned; closed-set for media hops |

## Code anchors (elsewhere)

| Piece | Location |
|-------|----------|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` |
| Custom circuit | `CircuitTunnelCoordinator`, circuit L4 |
| DialBack / Reachability / UPnP | `src/domain/mesh/reachability/` |
| Amp underlay | pp-cpp-amp (`PeerLink`, keepalive, `MaybeLearnPath`) |
| Hop reach helper | `AmpCircuitHopReach` |
| Partition compose tests | `src/domain/mesh/tests/` (`amp_circuit_*_compose_test`, loopback fixture) |
| Hard lab (forced A↛B nets) | Design: [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md); delivery [hard-lab](../hard-lab/) — not implemented |

## Next

1. **L3.25a** — Amp Coordinated Punch cold path (seed introducer) — [HOLE_PUNCH.md](HOLE_PUNCH.md)
2. **L3.5** — multi-hop circuit v2 when transitive reachability is needed (R1↛B, R2 can) — parallel
3. Mesh invest: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup)
