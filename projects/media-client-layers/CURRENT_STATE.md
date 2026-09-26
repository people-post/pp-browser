# Media client layers — current state

**Last updated:** 2026-09-26
**Branch:** `refactor/peer-reach-coordinator` (stacked — [L007](DECISIONS.md#l007--stack-on-the-l0-branch))

## Landed

| Phase | State |
|-------|-------|
| l0 — 1:1 split | Done: `PeerReachCoordinator`, `CallMediaConnectCoordinator` (both directions), bridge = call policy; stop / retry on UI; glare antisymmetric; SFU attach completion on UI; hard-lab COLD phases |
| l1 — reach in `domain/mesh` | Move done: `domain/mesh/reachability/{MeshReachPorts.h, PeerReachCoordinator, AmpCircuitHopReach}`, neutral names. Hop reach not yet routed through `PeerReachCoordinator` |

## Next

**l1 (rest)** — route hop reach (topology, `CallMediaPlane` hop paths) through `PeerReachCoordinator` where behavior matches; record the differences.

## Open questions

- Viewer ticket: fetched from the publisher 1:1 (a second reach) or delivered in the announce? (decides l4's reach count)
- `MediaRelayTypes` `call_id` rename: wire field or internal only ([L006](DECISIONS.md#l006--same-media_relay-data-plane-own-aead-label-per-feature))
- Seat vs arbiter: does `CallMediaSeat` keep call-path epochs only once `DeviceArbiter` owns devices (l3)?

## Agent traps

| Wrong | Right |
|-------|-------|
| Add broadcast logic to `CallTopologyController` / call stores | New code goes in `feature/broadcast` ([L001](DECISIONS.md#l001--broadcast-is-a-sibling-feature-of-calls-not-part-of-the-call-path)) |
| Shared layer API mentions call / program / ticket | Opaque session id + credentials ([L002](DECISIONS.md#l002--shared-layers-must-be-feature-neutral)) |
| Viewer starts capture | Playback-only session + speaker lease ([L003](DECISIONS.md#l003--device-leases-per-kind-exclusive-policy-for-now)) |
| Hard-code audio-only in stream plans | Channels generic; audio first ([L004](DECISIONS.md#l004--stream-plans-carry-channels-generically)) |
