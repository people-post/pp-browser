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
- [x] `BroadcastViewerWorkflow` (UI thread at the time — now the media-sessions owner, thread-ownership t2b-4; `DeferredSelf` generations): reach publisher → ticket → verify / extract key → `BroadcastViewerLadder` → receive-only attach via `AttachToMediaRelayAsync` (paid quotes declined) → subscribe the publisher stream (audio) → playback-only engine session (speaker lease); re-admit on relay loss (backoff; fails after 3 consecutive); Stop detaches, stops playback, drops late completions — a late successful attach after Stop is detached
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

### l5a — `BroadcasterWorkflow` (done)

- [x] Go live: fresh 32-byte key + join handle per show (B004) → program key to the ticket server (port) → attach to the first reachable relay via `AttachToMediaRelayAsync` (publish-only quote, paid declined) → capture-only engine (mic lease) whose frames are sealed under `broadcast-media|program|join` and sent on `BroadcastPublisherStreamId(self)` → Live tip (hop + L1 hints)
- [x] Relay loss: re-attach same hop first (tip stays valid), else the next hop + re-announce + re-point tickets; fails after 3 consecutive losses. End: Ended tip, key cleared, detach, capture stopped; a late attach after End is detached; never announces a show no relay accepted
- [x] gtests (fake relay, real capture engine on a device-less arbiter; frames opened with the viewer's label)

### l5b — Product wiring (done)

- [x] `BroadcastHub` owns the broadcaster (own capture engine) next to the viewer; `ForMesh` ports: `MeshDeliveryOrchestrator::PutLiveProgramKey` / `ClearLiveProgramKey` (serving `AmpBroadcastTransport`), `PublishAnnounceTip` (`PeerAnnouncePublisher`, local feed — pushing to followers is Spine D), `NewBroadcastMediaKey` / `NewBroadcastJoinHandle`
- [x] Facade `GoLive` / `EndLive` / `BroadcastLiveStatus`
- [x] Compose test `BroadcasterToRelayToViewerEndToEnd`: real broadcaster (capture engine, real ticket server) → relay → real viewer from the announced tip; after End a late viewer is refused a ticket
- [x] Found + fixed on the way: relay client send deadlock + off-io race + one-way session eviction (own commit); pp-cpp-amp v2.3.1 (dropped link closes its channel sessions — use-after-free) and v2.3.2 (thread-safe test network / clock)

### l5c — Hard lab (done)

- [x] **B-HARD-BCAST-NAT** (`--suite hard-w5 --phase broadcast`, in `all`): `pp-call-probe --role broadcaster` (peer-a, behind SNAT) goes live through the pp-node hop; two `--role viewer` probes (peer-b, behind the other SNAT) get the tip from `/share`, a ticket from the publisher over a relay circuit, a prompt admission refusal from the hop (pp-cpp-amp v2.3.0) and ≥100 decoded frames each — product `BroadcastHub` on `ProductStackHarness`
- [ ] Redirect case: needs a relay that serves admission (peer-scoped-broadcast B1); viewers behind separate NATs (a third lab peer)

**Exit (l5):** a broadcaster publishes and viewers listen end to end — compose-tested and hard-lab green. **Met.**

## l6 — Remove broadcast from calls (done)

- [x] Deleted `BroadcastSessionCoordinator`, `CallTopologyController::OnAnnounceViewerJoined`, `ArmJoinFromLiveAnnounce` / `AcceptLiveAnnounceJoin` (CSM, `CallUiBackend`), the facade / orchestrator `Plan*` / `Arm*` / `JoinLiveAnnounceFromTip` entry points, `AnnounceLiveJoin.*` and `ApplyBroadcastJoinTicket`; SoftMigrate has no `is_broadcast` input. The only watch entry is `WatchLiveAnnounce` → `BroadcastHub`
- [x] `CallSessionKind::Broadcast` stays a parsed legacy value only: nothing creates it, and `AcceptInvite` refuses such rows (older builds may have persisted them)
- [x] peer-scoped-broadcast CURRENT_STATE / PHASES point at `feature/broadcast`

**Exit (l6):** calls hold no broadcast code; `check_feature_includes.sh` bans the edge both ways. **Met.**

## l7 — Call-side cleanup (after the split)

Leftovers in `CallMediaBridge` / `CallTopologyController` / `CallHopMigrateWorkflow` found once broadcast was out.

- [x] Calls watch relay session ends through their own `AddClientTransportLostObserver` (reattach on `TransportLost` only), moved on every `SetMediaRelayDeps` and dropped by a `DeferredSelf` token at destruction. The call-owned `SetClientTransportLostHandler` slot is gone from `IMediaRelayClient` / `AmpMediaRelayClient` / `AmpMediaRelayCoordinator` (it was a raw `this` never unregistered, and a call concept in a shared interface)
- [x] Hop attach split: `AttachLocalToSfuAsync` (claim flight → media key → local hop / relay) and `CompleteHopAttach` (still-wanted check → already-live / superseded / stale-owner → `StartHopMedia` → `MarkHopAttachLive` → release direct) over one `HopAttach`; the second arming / cancel-gen check before StartSfu (same UI turn as the first) is gone
- [x] Topology handlers split: `OnInboundSfuAttach` (expect → settle without dial → start → current / superseded finish), `OnLocalAcceptJoined` (invite hint / group without hint / stay direct), `OnRemoteAcceptJoined`, `OnInboundSfuAttachFailed` (hop-hint re-pick); `ClaimMigrateFlight` / `ReleaseMigrateFlight` replace the copied generation bump and flight resets
- [x] Bridge split: `BeginSession` (reset → stop prior → seed park → engine → direct connect), `ScheduleStartMediaAsAnswerer` (UI start, media-key wait, worker poll, timeout); guest reattach reuses the `HopAttach` pieces
- [x] Found by the hard lab on the way: the media plane replaced its relay client under the topology (mesh start / rewire / reset); with the session-end observer the topology then unregistered from a destroyed client (SIGSEGV, B-HARD-CALL-NAT-STACK). `CallStack::UnbindRelayDependents` clears the topology's relay deps first; `ProductStackHarnessTest` (loopback product stack) guards it. hard-w5 `all` green after l7
- [x] `CallMediaPlane::ReserveOnBootstrapSeedsOnIo` (~160) / `EnsureBootstrapSeedParkedAsync` (~100) — split as they moved to `MeshMediaPlane` (l8)
- [ ] TSan: `CallSessionManager::BindWorkflowHostPorts` rewrites the workflow's port `std::function`s on UI (`SetDirectMediaPorts` / `SetLifecyclePorts`, e.g. mesh stop) while call workers (`HandleInboundAccept`, `AcceptInvite`) call them — ~1.3k reports across `CallSessionInboundComposeTest` / `CallUiBackendStackTest`, identical before l6. Needs ports that are bound once (or swapped atomically) — calls threading work
- [x] Blocking wrappers (`MaybeSoftMigrateToSfu` / `AttachLocalToSfu` / `ReattachGuestSfuTransport`, parking up to 60 s) removed from the workflow and topology — tests drive the async forms through a fixture `AwaitFlow`; dead topology forwarders (`ReattachGuestSfuTransport*`, `CompleteAttachLocalToSfu`) removed
- [x] `MaybeSoftMigrateToSfuAsync` split into named steps (arming gate → decide / re-pick → rank → `TryPickHop` / `AttachPickedHop`); the hop pick is a `HopPick` passed by `shared_ptr`. It used to be a `shared_ptr<function>` capturing itself — every SoftMigrate leaked its pick (the 14 LeakSanitizer failures in `CallTopologyControllerTest`); `SoftMigrateReleasesItsHopPickWhenSettled` guards it

## l8 — Neutral mesh media plane ([L015](DECISIONS.md#l015--a-neutral-meshmediaplane-in-domainmesh-owned-by-the-product-hub-lent-to-calls-and-broadcast))

- [x] `domain/mesh/media_plane/MeshMediaPlane`: media_relay client, dial registry + listen book, circuit reach + punch, rendezvous parking; hop candidates by port (`feature/conversations/MeshMediaPlaneWiring` over `MeshHopPolicy`); calls' R1 announce / signaling punch are hooks. `CallMediaPlane` keeps the call_media transport, bridge and topology relay deps
- [x] `ConversationsHub` / `ProductStackHarness` own it; `CallStack` borrows it (`CallStackDeps::mesh_media`, `DetachMeshMedia` / `RebindMeshMedia`); broadcast takes `RelayAttachPorts` from it; the owner runs the mesh-start, capability-refresh and stop sequences; `SharedRelayAttachPorts` / `CallStack::StopMesh` / the stack's reach wrappers removed; L014 superseded. l8a and l8b landed together (calls cannot include the conversations wiring)
- [x] Parking flows split while moving; the cold-reserve walk no longer holds itself (`shared_ptr<function>` self-capture leaked every reserve pass), a pending park still answers `false` at its deadline after mesh stop
- [x] `MeshMediaPlaneTest` (listen book, no-mesh wiring); `ProductStackHarnessTest` drives the owner's rewire sequence
- [x] Reach mechanics out of the plane into `domain/mesh/reachability`: `PunchIntroducerWalk` (the punch step reach calls — introducer walk, B29 next-introducer, H012 signaling fallback, upgrade punch) and `CircuitRendezvousCoordinator` (one relay surface: `DialableRelayIds` for circuit reach, warm / reserve / late reserve / park-await / re-park for inbound). `MeshMediaPlane` is composition + lifecycle only (`Rendezvous()` accessor)
- [ ] LeakSanitizer in untouched Amp code, seen once the punch / circuit suites ran under ASan: `AmpPunchCoordinator` (`RunIntroducerConnect` / `TryUpgradePunch` closures — `AmpPunchCoordinatorTest` ×6, `AmpPunchCircuitUpgradeTest`), circuit bridge (`AmpCircuitHopReachTest` ×7), `CallMediaLegCoordinator` (known). Likely the same self-owning-closure pattern fixed in l7 / l8

## Later

- [x] pp-cpp-amp v2.3.0: refuse channel opens for protocols without a handler (opt-in; `MeshHost` enables it) — admission to a plain relay fails at once. The 1.5 s admission timeout stays as the backstop for relays on older Amp
- [ ] `AmpChatBlobTransport` inbound handler has the same session-holder cycle the broadcast server had (not fixed here — out of scope)

- [ ] Viewer video (channel 1 decode path; subscribe plan adds channel)
- [ ] Tree relay upstream leg uses the viewer client ([L005](DECISIONS.md#l005--a-tree-relay-pulls-upstream-with-the-viewer-client)) — with peer-scoped-broadcast B1
- [ ] Relay per-program keyframe cache (relay side)
- [ ] Allow call + broadcast at once: per-holder `media_relay` client sessions (the coordinator holds one today), arbiter mic policy. Until then calls ignore `Replaced` / `Detached` (their own migrations and leaves cause those too), so a call does not notice another feature taking the client
