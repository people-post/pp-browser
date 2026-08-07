# Media hop reachability — current state

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
| **L4 SoftMigrate consume** | Rank hops; skip undialable after circuit; drop empty contact ma — **loopback compose green**; device SoftMigrate dogfood open |
| **Device LAN gates** | Direct 1:1 (s2b); 3-party circuit 1:1; SoftMigrate PreferLocal (s3c) — see [p2p-av-calls CURRENT_STATE](../p2p-av-calls/CURRENT_STATE.md) |
| **L3.5 multi-hop circuit** | Spec done — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md); **deferred** until single-hop device dogfood exposes “R1 cannot dial B” |
| **L5 directory / DHT** | Planned; closed-set for media hops |

## Code anchors (elsewhere)

| Piece | Location |
|-------|----------|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` |
| Custom circuit | `CircuitRelayService` |
| DialBack / Reachability / UPnP | mesh integration |
| Identify | cpp fork |
| Partition compose tests | `src/base/people/tests/circuit_*_compose_test.cpp` |

## Next

1. Device LAN dogfood gates (3-party circuit 1:1 + SoftMigrate PreferLocal)
2. **L3.5** — multi-hop circuit v2 only after single-hop dogfood shows transitive need

Track mesh invest: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup).
