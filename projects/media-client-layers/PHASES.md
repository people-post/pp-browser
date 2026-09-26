# Media client layers — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md).

```
l0 (1:1 split, done) ── l1 (reach → domain/mesh) ── l2 (relay attach) ──┬── l4 (broadcast viewer) ── l5 (broadcaster) ── l6 (remove from calls)
                                                   l3 (engine spec + device leases) ─┘
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

## l2 — MediaRelayAttachCoordinator

- [ ] Extract quote → `AcceptAndAttach` → reader → subscribe + reattach-on-loss from `CallHopMigrateWorkflow` into `domain/mesh` (UI-thread state, generation, hooks; shape of `CallMediaConnectCoordinator`)
- [ ] Stream plan input: `{publish?, subscribe streams, channels}` — no call / roster knowledge
- [ ] `MediaRelayTypes` `call_id` → neutral session id (wire field name decision recorded; compat if on the wire)
- [ ] Group joiner switches to it; topology / hop-migrate tests + hard-lab green

**Exit:** a group joiner attaches through the neutral coordinator.

## l3 — Engine session spec + device leases

- [ ] Engine session spec: duplex / capture-only / playback-only; channels generic (0 = Opus, 1 = H264 …)
- [ ] `DeviceArbiter`: leases per kind (mic / camera / speaker); policy exclusive with a clear refusal reason
- [ ] No singleton assumption: pipelines are instances owned by the lease holder; remove "one engine serves one call" stops of other sessions
- [ ] Calls use duplex through the arbiter; behavior unchanged (gtests + hard lab + dogfood)

**Exit:** a capture-only or playback-only session can run without starting the other half.

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
