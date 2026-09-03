# Media hop reachability — current state

> **2026-09:** Product mesh is Amp-only; hop reachability uses `AmpCircuitHopReach` + `MeshHost::CircuitDeps()`. libp2p fork removed — see [MESH.md](../../docs/architecture/MESH.md).

**Last updated:** 2026-08-07

## Direction

Hop **reachability** = **libp2p stack work** (H001/H007). App-layer `call_hop_addrs` prototype **removed** (uncommitted); do not re-land without ADR.

**Spec:** [DESIGN.md](DESIGN.md) (concept-first). **Build order:** [PHASES.md](PHASES.md).

## Landed

| Area | State |
|------|-------|
| Project docs | Ownership: fork implements; SoftMigrate consumes |
| ADRs | H001–H008; circuit multi-hop plan [N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker) |
| **L1 peer address book** | `PeerAddressBook`, `PeerSessionManager` — upsert on bootstrap/register/connect/dial-success; `PreferredPeerMultiaddr` |
| **L2 advertised listen set** | `BuildAdvertisedListenSet`, `IdentifyIntegrationService`, `AdvertisedAddrPublisher` |
| **L3 circuit PeerId dial** | `CircuitBridgeTarget`, `TryEnsureHopViaCircuit` — **single-hop**; SoftMigrate circuit fallback via `ICircuitHopReach` |
| **L3 compose (loopback)** | Shared `loopback_partition_fixture.h`; `CircuitCallMediaComposeTest` (call-media via R); `CircuitMediaRelayComposeTest` (quote/attach/fan-out via R); `IsReachableForProtocol` |

## In progress / gaps

| Area | State |
|------|-------|
| **L4 SoftMigrate consume** | Rank hops; skip undialable after circuit; drop empty contact ma — **loopback compose green** |
| **L3.5 multi-hop circuit** | Spec done — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md); later when single-hop cannot reach B |
| **L5 directory / DHT** | Planned; closed-set for media hops |

## Code anchors (elsewhere)

| Piece | Location |
|-------|----------|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` |
| Custom circuit | `CircuitRelayService` |
| DialBack / Reachability / UPnP | mesh integration |
| Identify | cpp fork |
| Partition compose tests | `src/domain/mesh/tests/` (`circuit_*_compose_test`, loopback fixture) |
| Hard lab (forced A↛B nets) | Design: [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md); delivery [hard-lab](../hard-lab/) — not implemented |

## Next

1. **L3.5** — multi-hop circuit v2 when transitive reachability is needed (R1↛B, R2 can)
2. Mesh invest: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup)
