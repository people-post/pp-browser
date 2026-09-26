# Media client layers — current state

**Last updated:** 2026-09-26
**Branch:** `refactor/peer-reach-coordinator` (stacked — [L007](DECISIONS.md#l007--stack-on-the-l0-branch))

## Landed

| Phase | State |
|-------|-------|
| l0 — 1:1 split | Done: `PeerReachCoordinator`, `CallMediaConnectCoordinator` (both directions), bridge = call policy; stop / retry on UI; glare antisymmetric; SFU attach completion on UI; hard-lab COLD phases |
| l1 — reach in `domain/mesh` | Done: `domain/mesh/reachability/{MeshReachPorts.h, PeerReachCoordinator, AmpCircuitHopReach}`, neutral names. Hop reach stays a separate *service reach* ([L008](DECISIONS.md#l008--two-kinds-of-reach-link-reach-and-service-reach)) |
| l2 — relay attach | Done: `domain/mesh/l4/media_relay/{IMediaRelayClient.h, AmpMediaRelayClient, MediaRelayAttach}`; group attach + guest reattach use `AttachToMediaRelayAsync` ([L009](DECISIONS.md#l009--relay-attach-is-a-stateless-capability-recovery-stays-with-each-feature)); wire `call_id` unchanged |
| l3b — device arbiter (audio) | Done: `MediaDeviceArbiter` device thread + `AudioDeviceLease` (mic exclusive, speaker shared — [L011](DECISIONS.md#l011--speaker-is-shared-mic-exclusive-one-device-thread-either-way)); engine + ringtone hold leases; quit drains closes before `SDL_Quit` |
| l3a — session spec | Done: `CallMediaEngine::SessionSpec` + `Start(session_id, spec, send)` (`StartSfu` = duplex); playback-only never opens the mic or sends; capture-only never opens playback; encoder created with the camera; `media_session_spec_test` ([L010](DECISIONS.md#l010--l3-splits-spec-first-then-a-device-owner-thread)) |

## Next

**l3b-2** — camera lease on the device thread (`SDL_OpenCamera` still runs under the engine mutex), then **l4** (viewer) — l4 needs only l3a + audio leases, so it can go first. Dogfood the audio arbiter on Android (speaker toggle reopen) and macOS (mic prompt) before a release.

## Open questions

- Viewer ticket: fetched from the publisher 1:1 (a second reach) or delivered in the announce? (decides l4's reach count)
- Seat vs arbiter: does `CallMediaSeat` keep call-path epochs only once `DeviceArbiter` owns devices (l3)?

## Agent traps

| Wrong | Right |
|-------|-------|
| Add broadcast logic to `CallTopologyController` / call stores | New code goes in `feature/broadcast` ([L001](DECISIONS.md#l001--broadcast-is-a-sibling-feature-of-calls-not-part-of-the-call-path)) |
| Shared layer API mentions call / program / ticket | Opaque session id + credentials ([L002](DECISIONS.md#l002--shared-layers-must-be-feature-neutral)) |
| Open SDL audio devices in a pipeline | Take an `AudioDeviceLease` from the injected `MediaDeviceArbiter` |
| `Start` a session and expect a video encoder to exist | Encoder is created by `SetCameraEnabled` (capturing sessions only) |
| Viewer starts capture | Playback-only session + speaker lease ([L003](DECISIONS.md#l003--device-leases-per-kind-exclusive-policy-for-now)) |
| Hard-code audio-only in stream plans | Channels generic; audio first ([L004](DECISIONS.md#l004--stream-plans-carry-channels-generically)) |
