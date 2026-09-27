# Media client layers — current state

**Last updated:** 2026-09-26
**Branch:** `refactor/peer-reach-coordinator` (stacked — [L007](DECISIONS.md#l007--stack-on-the-l0-branch))

## Landed

| Phase | State |
|-------|-------|
| l0 — 1:1 split | Done: `PeerReachCoordinator`, `CallMediaConnectCoordinator` (both directions), bridge = call policy; stop / retry on UI; glare antisymmetric; SFU attach completion on UI; hard-lab COLD phases |
| l1 — reach in `domain/mesh` | Done: `domain/mesh/reachability/{MeshReachPorts.h, PeerReachCoordinator, AmpCircuitHopReach}`, neutral names. Hop reach stays a separate *service reach* ([L008](DECISIONS.md#l008--two-kinds-of-reach-link-reach-and-service-reach)) |
| l2 — relay attach | Done: `domain/mesh/l4/media_relay/{IMediaRelayClient.h, AmpMediaRelayClient, MediaRelayAttach}`; group attach + guest reattach use `AttachToMediaRelayAsync` ([L009](DECISIONS.md#l009--relay-attach-is-a-stateless-capability-recovery-stays-with-each-feature)); wire `call_id` unchanged |
| l5b — broadcaster wiring | Done: hub owns the broadcaster; facade `GoLive` / `EndLive`; broadcaster → relay → viewer compose test green (TSan / ASan clean); pp-cpp-amp v2.3.2 pinned |
| l5a — broadcaster workflow | Done: `BroadcasterWorkflow` (go live → key to ticket server → relay attach → capture-only sealed frames → Live tip; re-attach / re-announce; End) on ports, gtests |
| l4c — product wiring | Done: `AmpBroadcastRpcClient`; `BroadcastHub::ForMesh` owned by `ConversationsHub` on borrowed call-plane mesh objects ([L014](DECISIONS.md#l014--broadcast-borrows-the-call-planes-mesh-objects-until-a-neutral-mesh-media-plane-exists)); facade `WatchLiveAnnounce`; three-node compose test (ticket → relay → playback). No UI entry yet |
| l4b — `feature/broadcast` | Done: `BroadcastHub` + `BroadcastViewerWorkflow` on ports (fakes in gtests); relay loss observers; sibling include ban in CI |
| l4a — frame crypto + ladder | Done: neutral `MediaRelayFrameCrypto`; `BroadcastViewerLadder`; broadcast stream id / frame context ([L013](DECISIONS.md#l013--viewer-shape-ticket-from-the-publisher-client-side-ladder-direct-attach-to-hops-without-admission)) |
| l3b — device arbiter | Done (audio + camera): `MediaDeviceArbiter` device thread + `AudioDeviceLease` (mic exclusive, speaker shared — [L011](DECISIONS.md#l011--speaker-is-shared-mic-exclusive-one-device-thread-either-way)); engine + ringtone hold leases; camera async on the video thread ([L012](DECISIONS.md#l012--camera-requests-are-asynchronous-the-video-thread-owns-camera-and-encoder)); quit drains closes before `SDL_Quit` |
| l3a — session spec | Done: `CallMediaEngine::SessionSpec` + `Start(session_id, spec, send)` (`StartSfu` = duplex); playback-only never opens the mic or sends; capture-only never opens playback; encoder created with the camera; `media_session_spec_test` ([L010](DECISIONS.md#l010--l3-splits-spec-first-then-a-device-owner-thread)) |

## Next

**l5c** — hard lab: a broadcaster probe → pp-node relay → ≥2 viewers. Open alongside: watch / go-live UI entry (no gui caller yet); neutral mesh media plane (L014 exit); followers learn Live tips only by pull / explicit push (Spine D). Before a release: dogfood the device arbiter (PHASES l3b).

Known, not from this work (seen 2026-09-26): `--suite node` / `call-hop` fail on this lab machine — pp-node's advertised listen switches to the seed-observed `172.126.x` address, unreachable from the host (same with or without the Amp change); `COLD-DIRTY` nested-circuit dial timeout still intermittent (1 of 3 hard-w5 runs); `AmpCircuitCallMediaComposeTest` leaks a `CallMediaLegCoordinator::Impl` under LeakSanitizer (identical on Amp v2.3.0 and v2.3.2).

## Open questions

- Seat vs arbiter: does `CallMediaSeat` keep call-path epochs only once `DeviceArbiter` owns devices (l3)?

## Agent traps

| Wrong | Right |
|-------|-------|
| Add broadcast logic to `CallTopologyController` / call stores | New code goes in `feature/broadcast` ([L001](DECISIONS.md#l001--broadcast-is-a-sibling-feature-of-calls-not-part-of-the-call-path)) |
| Shared layer API mentions call / program / ticket | Opaque session id + credentials ([L002](DECISIONS.md#l002--shared-layers-must-be-feature-neutral)) |
| Open SDL audio devices in a pipeline | Take an `AudioDeviceLease` from the injected `MediaDeviceArbiter` |
| `Start` a session and expect a video encoder to exist | The video thread creates it when the camera lease opens (capturing sessions only) |
| Expect `SetCameraEnabled(true)` to have opened the camera | It only records the request; poll `IsCameraEnabled` / `TakeCameraFailure` |
| Viewer starts capture | Playback-only session + speaker lease ([L003](DECISIONS.md#l003--device-leases-per-kind-exclusive-policy-for-now)) |
| Hard-code audio-only in stream plans | Channels generic; audio first ([L004](DECISIONS.md#l004--stream-plans-carry-channels-generically)) |
