# Media hop reachability — current state

**Last updated:** 2026-08-01

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

## In progress / gaps

| Area | State |
|------|-------|
| **L4 SoftMigrate consume** | Rank hops as today; skip undialable; drop empty contact ma as only dial signal |
| **L3.5 multi-hop circuit** | Spec done — [MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md); not implemented |
| **L5 directory / DHT** | Planned; closed-set for media hops |

## Code anchors (elsewhere)

| Piece | Location |
|-------|----------|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` |
| Custom circuit | `CircuitRelayService` |
| DialBack / Reachability / UPnP | mesh integration |
| Identify | cpp fork |

## Next

1. **L4** — SoftMigrate consume stack dialability only  
2. **L3.5** — multi-hop circuit v2  

Track mesh invest: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup).
