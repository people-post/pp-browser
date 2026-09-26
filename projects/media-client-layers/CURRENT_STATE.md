# Media client layers — current state

**Last updated:** 2026-09-26
**Branch:** `refactor/peer-reach-coordinator` (stacked — [L007](DECISIONS.md#l007--stack-on-the-l0-branch))

## Landed

| Phase | State |
|-------|-------|
| l0 — 1:1 split | Done: `PeerReachCoordinator`, `CallMediaConnectCoordinator` (both directions), bridge = call policy; stop / retry on UI; glare antisymmetric; SFU attach completion on UI; hard-lab COLD phases |
| l1 — reach in `domain/mesh` | Done: `domain/mesh/reachability/{MeshReachPorts.h, PeerReachCoordinator, AmpCircuitHopReach}`, neutral names. Hop reach stays a separate *service reach* ([L008](DECISIONS.md#l008--two-kinds-of-reach-link-reach-and-service-reach)) |
| l2 — relay attach | Done: `domain/mesh/l4/media_relay/{IMediaRelayClient.h, AmpMediaRelayClient, MediaRelayAttach}`; group attach + guest reattach use `AttachToMediaRelayAsync` ([L009](DECISIONS.md#l009--relay-attach-is-a-stateless-capability-recovery-stays-with-each-feature)); wire `call_id` unchanged |
| l3a — session spec | Done: `CallMediaEngine::SessionSpec` + `Start(session_id, spec, send)` (`StartSfu` = duplex); playback-only never opens the mic or sends; capture-only never opens playback; encoder created with the camera; `media_session_spec_test` ([L010](DECISIONS.md#l010--l3-splits-spec-first-then-a-device-owner-thread)) |

## Next

**l3b** — `DeviceArbiter` leases (exclusive policy) on a device owner thread that takes SDL open / close / reopen away from the capture thread ([L010](DECISIONS.md#l010--l3-splits-spec-first-then-a-device-owner-thread)); calls keep behavior. Threading change: own commits, TSan media + call suites, hard lab + dogfood. l4 needs only l3a, so it can go first if l3b stalls.

## Open questions

- Viewer ticket: fetched from the publisher 1:1 (a second reach) or delivered in the announce? (decides l4's reach count)
- Seat vs arbiter: does `CallMediaSeat` keep call-path epochs only once `DeviceArbiter` owns devices (l3)?

## Agent traps

| Wrong | Right |
|-------|-------|
| Add broadcast logic to `CallTopologyController` / call stores | New code goes in `feature/broadcast` ([L001](DECISIONS.md#l001--broadcast-is-a-sibling-feature-of-calls-not-part-of-the-call-path)) |
| Shared layer API mentions call / program / ticket | Opaque session id + credentials ([L002](DECISIONS.md#l002--shared-layers-must-be-feature-neutral)) |
| `Start` a session and expect a video encoder to exist | Encoder is created by `SetCameraEnabled` (capturing sessions only) |
| Viewer starts capture | Playback-only session + speaker lease ([L003](DECISIONS.md#l003--device-leases-per-kind-exclusive-policy-for-now)) |
| Hard-code audio-only in stream plans | Channels generic; audio first ([L004](DECISIONS.md#l004--stream-plans-carry-channels-generically)) |
