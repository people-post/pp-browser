# Media hop reachability — current state

**Last updated:** 2026-08-01

## Direction

Hop **reachability** = **libp2p stack work** (H001/H007). App-layer `call_hop_addrs` prototype **removed** (uncommitted); do not re-land without ADR.

## Landed (docs)

| Area | State |
|------|-------|
| Project docs | Ownership: fork implements; SoftMigrate consumes |
| ADRs | H001–H008; circuit multi-hop plan [N024](../p2p-mesh/DECISIONS.md#n024--immediate-relay-as-service-broker) |

## Code today (elsewhere)

| Piece | Location | Gap |
|-------|----------|-----|
| SoftMigrate hop pick | `MeshHopPolicy`, `CallTopologyController` | Needs contact/seed **multiaddr** string |
| Custom circuit | `CircuitRelayService` | Still requires **target_multiaddr** |
| DialBack / Reachability / UPnP | mesh integration | Not yet a unified peerstore for arbitrary hop PeerIds |
| Identify | cpp fork | Present; not fully driving SoftMigrate dial |
| **L1 peer address book** | `PeerAddressBook`, `PeerSessionManager` | Upsert on bootstrap/register/connect/dial-success; `PreferredPeerMultiaddr` for hop dial |
| **L2 advertised listen set** | `BuildAdvertisedListenSet`, `IdentifyIntegrationService`, `AdvertisedAddrPublisher` | Identify wired; Node+media_relay publishes probe-derived addrs |
| **L3 circuit PeerId dial** | `CircuitBridgeTarget`, `PeerSessionManager::TryEnsureHopViaCircuit` | Bridge by `target_peer_id`; SoftMigrate circuit fallback via `ICircuitHopReach` — **single-hop only** ([MULTI_HOP_CIRCUIT.md](MULTI_HOP_CIRCUIT.md)) |

## Next

1. **L4** SoftMigrate consume  

Track mesh invest: [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup).
