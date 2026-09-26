# Media client layers — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md).

```
l0 (done) ── l1 (reach → domain/mesh, done) ── l2 (relay attach, done) ──┬── l4 (broadcast viewer) ── l5 (broadcaster) ── l6 (remove from calls)
                                l3a (spec, done) ── l3b (device leases) ── l3c? ─┘
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

### l3b — `DeviceArbiter` on a device owner thread

- [ ] `DeviceArbiter`: leases per kind (mic / camera / speaker); policy exclusive with a clear refusal reason; injectable, process default
- [ ] One device thread owns SDL audio / camera open, close and reopen-on-loss; pipelines get streams through leases, never open devices (replaces the capture thread's device ownership)
- [ ] Calls take duplex leases; behavior unchanged (gtests + hard lab + dogfood)

### l3c — Pipeline instances (only if l4 / l5 need it)

- [ ] No singleton assumption: pipelines are instances owned by the lease holder; remove "one engine serves one call" stops of other sessions; split / rename `CallMediaEngine` if the call name gets in the way

**Exit:** a capture-only or playback-only session can run without starting the other half. **Met by l3a** (l3b / l3c are about sharing devices between sessions).

## l4 — feature/broadcast: viewer (audio)

- [ ] `feature/broadcast` + `BroadcastHub`; own program / subscription state (not `CallSessionStore`), no `CallLifecycle`
- [ ] `BroadcastViewerWorkflow`: ticket → `RequestViewerAttach` admit-or-redirect (bounded) → receive-only attach via l2 → re-admit on loss
- [ ] Broadcast client RPC moves out of `AmpBroadcastTransport` (client vs relay-side split)
- [ ] Own frame AEAD label; playback-only engine session; speaker lease
- [ ] Hard-lab phase: publisher + relay + viewer (audio flows; redirect case)

**Exit:** a viewer watches (listens) without any call object.

## l5 — Broadcaster

- [ ] `BroadcasterWorkflow`: capture-only session (mic lease; camera optional input) → publish to first relay via l2
- [ ] Live key / ticket minting wired to the program (`LiveProgramKey`)
- [ ] Hard-lab: broadcaster → relay → ≥2 viewers

## l6 — Remove broadcast from calls

- [ ] Delete `BroadcastSessionCoordinator`, `CallTopologyController::OnAnnounceViewerJoined`, `ArmJoinFromLiveAnnounce` / `AcceptLiveAnnounceJoin`, `CallSessionKind::Broadcast` records in `CallSessionStore`, SoftMigrate `is_broadcast` skip
- [ ] peer-scoped-broadcast PHASES / CURRENT_STATE point at `feature/broadcast`

## Later

- [ ] Viewer video (channel 1 decode path; subscribe plan adds channel)
- [ ] Tree relay upstream leg uses the viewer client ([L005](DECISIONS.md#l005--a-tree-relay-pulls-upstream-with-the-viewer-client)) — with peer-scoped-broadcast B1
- [ ] Relay per-program keyframe cache (relay side)
- [ ] Allow call + broadcast at once (arbiter policy change + playback mix)
