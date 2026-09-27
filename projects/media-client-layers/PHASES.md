# Media client layers — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md).

```
l0 (done) ── l1 (reach → domain/mesh, done) ── l2 (relay attach, done) ──┬── l4 (broadcast viewer) ── l5 (broadcaster) ── l6 (remove from calls)
                                l3a (spec, done) ── l3b (audio + camera leases, done) ── l3c? ─┘
later: viewer video · relay upstream via viewer client · relay keyframe cache
```

l1 and l2 are refactors checked against existing tests + hard lab. l3 is the risky one (engine, SDL, UI-thread rules). l4+ is mostly new code nothing depends on yet.

## l0 — 1:1 split (done)

- [x] `PeerReachCoordinator` (link), `CallMediaConnectCoordinator` (bundle, both directions), `CallMediaBridge` (call policy) — branch `refactor/peer-reach-coordinator`
- [x] Hard-lab COLD / COLD-DIRTY / COLD-AWAIT drive the product reach

## l1 — Reach moves to domain/mesh

- [x] `PeerReachCoordinator` + its dial / circuit ports (`IDialRegistry`, `ICircuitHopReach`, `PeerSessionDialRegistry`, `CircuitHopReachClient` → `domain/mesh/reachability/MeshReachPorts.h`) and `AmpCircuitHopReach` move to `domain/mesh/reachability`; neutral names (`TryEnsurePeerReachable*`, `Has/ClearPeerCircuitHop`, `ShouldSkipPrivatePreferredDialAfterSeedPark`); no `domain → feature` include
- [x] Hop reach compared, not merged: it is *service reach* (media_relay dialable via protocol-keyed circuit hop), not link reach — [L008](DECISIONS.md#l008--two-kinds-of-reach-link-reach-and-service-reach); both now in `domain/mesh/reachability`
- [x] gtests move with the class (`peer_reach_coordinator_test` → `domain/mesh/tests`); full suite green

**Exit:** `domain/mesh` owns reaching any node (link reach + service reach); `feature/calls` only asks. **Met.**

## l2 — MediaRelayAttach (stateless capability — [L009](DECISIONS.md#l009--relay-attach-is-a-stateless-capability-recovery-stays-with-each-feature))

- [x] `IMediaRelayClient` → `domain/mesh/l4/media_relay/IMediaRelayClient.h`; `AmpMediaRelayClient` → `domain/mesh/l4/media_relay` (git mv); neutral `session_id` parameters
- [x] `AttachToMediaRelayAsync`: register dial hint → service reach when undialable → quote → `accept_quote` gate → `still_wanted` → AcceptAndAttach (ports + request + hooks; no call / roster knowledge); gtests `media_relay_attach_test`
- [x] `MediaRelayQuoteRequest::call_id` → `session_id` in C++; wire field stays `"call_id"` ([L006](DECISIONS.md#l006--same-media_relay-data-plane-own-aead-label-per-feature) resolved)
- [x] Group joiner attach **and** guest reattach use it (was copy-pasted twice); pricing gate + quote sizing + frame decrypt stay call policy
- [x] Full suite, TSan (topology / bridge / connect), hard-lab `all` green
- [ ] Gap: no hard-lab phase drives a group SFU attach through the product topology (covered by topology gtests with a fake relay + mesh `media_relay` compose tests) — add with l4's relay lab work
- [ ] Stream plan (`{publish?, subscribe streams, channels}`) — moved to l3 / l4 where the engine and viewer need it

**Exit:** a group joiner attaches through the neutral capability. **Met.**

## l3 — Engine session spec + device leases

Split in three ([L010](DECISIONS.md#l010--l3-splits-spec-first-then-a-device-owner-thread)): the engine's capture thread owns device open / reopen *and* send pacing, so the spec and the device ownership change are separate steps.

### l3a — Session spec on the existing engine (done)

- [x] `CallMediaEngine::SessionSpec` (`Duplex` / `CaptureOnly` / `PlaybackOnly`) + `Start(session_id, spec, send)`; `StartSfu` = duplex; `ActiveSpec()`
- [x] Capture-only: no playback device, no playout thread, inbound packets ignored. Playback-only: no mic, no VoIP audio-session activation, no send fn, camera refused
- [x] Local video encoder created with the camera, not the session (audio-only and viewer sessions never open VAAPI / VideoToolbox / MF)
- [x] Health snapshot reads engine-mutex fields under the lock; video target atomic (TSan, first engine-level tests)
- [x] gtests `media_session_spec_test` (engine-level, no devices); full suite, TSan media + call suites, hard-lab `hard-w5` green

### l3b — `MediaDeviceArbiter` on a device owner thread

- [x] `MediaDeviceArbiter` (`domain/media/MediaDeviceArbiter.*`): per-kind `AudioDeviceLease`s; policy per kind — mic exclusive (refusal names the holder), speaker shared ([L011](DECISIONS.md#l011--speaker-is-shared-mic-exclusive-one-device-thread-either-way)); injectable (`CallMediaEngine(MediaDeviceArbiter&)`, `CallRingtone(MediaDeviceArbiter&)`), process `Default()`
- [x] One device thread performs every audio open / close / reopen (`IAudioDeviceBackend`: SDL, null); holders do I/O through the lease, so an endpoint is never closed under a reader / writer; releases never block the holder
- [x] Engine takes the leases its spec asks for (duplex: mic + speaker; playback-only: speaker) and reopens them in place; ringtone holds a speaker lease — `CallRingtone::WaitUntilPlaybackDeviceReleased` and its global holder count are gone
- [x] Quit: `MediaDeviceArbiter::ShutdownDefault` after the runtime joins, before `SDL_Quit`
- [x] gtests `media_device_arbiter_test` (fake backend: exclusivity, shared speaker, single device thread, no overlapping OS calls, FIFO close-before-open, I/O during reopen, engine leases per spec); full suite, TSan / ASan media, TSan call suites, hard-lab `hard-w5` green
- [x] Camera lease (l3b-2, [L012](DECISIONS.md#l012--camera-requests-are-asynchronous-the-video-thread-owns-camera-and-encoder)): `CameraDeviceLease` (exclusive) opened on the device thread; `SetCameraEnabled` async (UI records request + display rotation; `CameraDisplayRotationDegrees` split from `ResolveCameraCaptureTransform`); video thread owns the camera lease and the local encoder; failures via `TakeCameraFailure` → `CallController` withdraws video; no join of the video thread under the engine mutex
- [x] `VideoEncoderAvailable` is a host capability again (`PlatformVideoEncoderSupported`) — l3a had tied it to the lazily created encoder, hiding the camera button
- [x] Linux VA-API: stage `vaPutImage` uploads at the 16-aligned surface size (edge-padded) — frame-sized staging overflowed the heap in radeonsi at 640×360 (found by the l3b-2 engine tests; `video_codec_encode_test`)
- [ ] Dogfood: camera on / off on iOS (SDL camera opened off the main thread now), Android (NDK), macOS (permission prompt), Windows (MF); Android speaker toggle / SoftMigrate reopen, macOS mic prompt, ring over an active call; Android leave — `CallAudioSession::Deactivate` can now run just before the device thread closes AudioRecord (closes are async)

### l3c — Pipeline instances (only if l4 / l5 need it)

- [ ] No singleton assumption: pipelines are instances owned by the lease holder; remove "one engine serves one call" stops of other sessions; split / rename `CallMediaEngine` if the call name gets in the way

**Exit:** a capture-only or playback-only session can run without starting the other half. **Met by l3a** (l3b / l3c are about sharing devices between sessions).

## l4 — feature/broadcast: viewer (audio) — [L013](DECISIONS.md#l013--viewer-shape-ticket-from-the-publisher-client-side-ladder-direct-attach-to-hops-without-admission)

### l4a — Neutral frame crypto + viewer ladder (done)

- [x] `MediaRelayFrameCrypto` (`domain/mesh/l4/media_relay`): seal / open with a feature-owned AAD context; call SFU framing delegates (bytes unchanged — cross-open test)
- [x] `BroadcastMediaFrameContext`, `BroadcastPublisherStreamId` (`domain/messaging/BroadcastMedia.h`)
- [x] `BroadcastViewerLadder` (`domain/messaging`): client side of B007 — admit / redirect (budget, path stamp) / refuse / no-admission-service / attach-failed; gtests

### l4b — `feature/broadcast` viewer workflow + hub (done)

- [x] `feature/broadcast` library (`pp_feature_broadcast`) + `BroadcastHub` (owns a playback engine + the viewer); own watch state, no `CallSessionStore` / `CallLifecycle`; `check_feature_includes.sh` bans calls ↔ broadcast includes
- [x] `BroadcastViewerWorkflow` (UI thread, `DeferredSelf` generations): reach publisher → ticket → verify / extract key → `BroadcastViewerLadder` → receive-only attach via `AttachToMediaRelayAsync` (paid quotes declined) → subscribe the publisher stream (audio) → playback-only engine session (speaker lease); re-admit on relay loss (backoff; fails after 3 consecutive); Stop detaches, stops playback, drops late completions — a late successful attach after Stop is detached
- [x] Relay transport-loss **observers** (`Add/RemoveClientTransportLostObserver`) next to the calls' handler slot, so both features hear losses without stealing each other's handler
- [x] gtests (fake ports, real ML-DSA tickets, real playback engine on a device-less arbiter): admit / redirect / no-admission / ticket hop / attach failure / refusal / ticket problems / relay busy / loss + give-up / stop / late attach / paid quote / hub; TSan + ASan clean

### l4c — Product wiring (done — [L014](DECISIONS.md#l014--broadcast-borrows-the-call-planes-mesh-objects-until-a-neutral-mesh-media-plane-exists))

- [x] `AmpBroadcastRpcClient` (`feature/broadcast`): client half split out of `AmpBroadcastTransport` (now serving-only); completions never touch the client; admission uses a short timeout (1.5 s — Amp acks opens for unhandled protocols, so a plain relay only answers by timeout)
- [x] `BroadcastHub::ForMesh` (RPC client + own `PeerReachCoordinator` for the publisher + viewer, UI via `AppRuntime`); `ConversationsHub` builds it after mesh services start, drops it before every relay rewire / mesh stop ([L014](DECISIONS.md#l014--broadcast-borrows-the-call-planes-mesh-objects-until-a-neutral-mesh-media-plane-exists)); `CallStack::SharedRelayAttachPorts`; `MeshDeliveryOrchestrator::ResolveAnnouncePublisherKey` (was duplicated) + `LatestAnnounceTip`
- [x] Facade: `WatchLiveAnnounce`, `WatchStoredLiveAnnounce`, `StopWatchingBroadcast`, `BroadcastWatchStatus`
- [x] Compose tests (`broadcast_viewer_compose_test`, three-node mesh): real ticket from the publisher's server → plain relay (no admission, timeout → direct attach) → publisher frames reach the viewer's playback engine; admitting hop admits; unknown program fails at the ticket
- [x] Found + fixed on the way (own commit): media_relay client loss handler wiped by every attach (guest reattach-on-loss dead after the first attach), Detach lock-order inversion, hop channel leak; broadcast RPC server + client session cycles (LeakSanitizer)
- [ ] UI: a watch surface (Notifications / announce banner → Watch / Stop, status line) — no gui caller yet

**Exit:** a viewer listens without any call object. **Met** (compose-tested end to end; product-wired; no UI entry yet). Hard-lab (publisher + relay + viewer, redirect case) moves to l5 (needs a real publisher).

## l5 — Broadcaster

- [ ] `BroadcasterWorkflow`: capture-only session (mic lease; camera optional input) → publish to first relay via l2
- [ ] Live key / ticket minting wired to the program (`LiveProgramKey`)
- [ ] Hard-lab: broadcaster → relay → ≥2 viewers (from l4: redirect case)

## l6 — Remove broadcast from calls

- [ ] Delete `BroadcastSessionCoordinator`, `CallTopologyController::OnAnnounceViewerJoined`, `ArmJoinFromLiveAnnounce` / `AcceptLiveAnnounceJoin`, `CallSessionKind::Broadcast` records in `CallSessionStore`, SoftMigrate `is_broadcast` skip
- [ ] peer-scoped-broadcast PHASES / CURRENT_STATE point at `feature/broadcast`

## Later

- [ ] Neutral mesh media plane: move the media_relay client, dial registry, circuit reach, punch and seed parking out of `CallMediaPlane` into `domain/mesh`, owned outside calls and lent to both features ([L014](DECISIONS.md#l014--broadcast-borrows-the-call-planes-mesh-objects-until-a-neutral-mesh-media-plane-exists) exit)
- [x] pp-cpp-amp v2.3.0: refuse channel opens for protocols without a handler (opt-in; `MeshHost` enables it) — admission to a plain relay fails at once. The 1.5 s admission timeout stays as the backstop for relays on older Amp
- [ ] `AmpChatBlobTransport` inbound handler has the same session-holder cycle the broadcast server had (not fixed here — out of scope)

- [ ] Viewer video (channel 1 decode path; subscribe plan adds channel)
- [ ] Tree relay upstream leg uses the viewer client ([L005](DECISIONS.md#l005--a-tree-relay-pulls-upstream-with-the-viewer-client)) — with peer-scoped-broadcast B1
- [ ] Relay per-program keyframe cache (relay side)
- [ ] Allow call + broadcast at once: per-holder `media_relay` client sessions (the coordinator holds one today), arbiter mic policy
