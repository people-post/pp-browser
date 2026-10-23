# P2P A/V calls — current state

**Last updated:** 2026-09-28

**North star:** [NETWORKING.md](../../docs/architecture/NETWORKING.md) + **[V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)** — HTTP + libp2p only; call media on libp2p (voice-first). **m2 done:** libdatachannel removed from build; wire-compat `call_sdp`/`call_ice` ignored.

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md) (**Call lifecycle**). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md). Cross-network B25–B31 triage: [CROSS_NETWORK_B25_B31.md](CROSS_NETWORK_B25_B31.md).

## Landed

| Area | State |
|------|-------|
| Project docs | a3 done; **a4 thin**; **V026** libp2p-only media |
| ADRs | V001–**V049** |
| **V035 SoftMigrate scope** | PreferLocal only for **LAN-confirmed Link**; Site/Wide → org seed; ignore stale CallSfuAttach/HopRefuse/**CallAccept** when another call is bound |
| **V036 MediaSeat** | **Phase 3 landed** — `CallDirectPath` / `CallHopPath` token façades; CSM signaling-only for duplex start/stop; Phase 1–2 bind/Live/attach flight retained — [DECISIONS V036](DECISIONS.md#v036--mediaseat--exclusive-media-epoch) |
| **V037 State+Status FSM** | `CallPhase` + `CallMediaStatus`; one planner armed per pair; `media_cancel_gen`; gates on ScheduleStart / CallSfuAttach / CompleteAttach — [DECISIONS V037](DECISIONS.md#v037--calllifecycle-state--status-one-planner-armed) |
| **V038 rewrite debt** | N=2 = direct → punch → **circuit** call-media; SoftMigrate / `media_relay` = **N≥3 only** — [DECISIONS V038](DECISIONS.md#v038--n2-circuit-for-nat-softmigrate-reserved-for-n3); phase [rd](PHASES.md#rd--amp-call-media-rewrite-debt-v038) |
| **N→planner select** | [`CallMediaPlannerSelectLogic`](../../src/lib/calls/CallMediaPlannerSelectLogic.h) + Topology `OnPeerMediaRelayCapLearned`; CSM Accept no longer owns SoftMigrate nudge trees |
| **V039 planner FSMs** | **pm0–pm4 landed** — Direct/Hop `Apply` + logic gtests; health/attach-wait SM timers; CALLS race homes → planner phases — [DECISIONS V039](DECISIONS.md#v039--call-directhop-planner-machines) |
| **V040 CallMediaPlane** | **Landed (cs0–cs2)** — `CallMediaPlane` owns Amp/dial/relay/hop/bridge/dial book; Lifecycle sole N025 desire; CALLS.md ownership promoted — [DECISIONS V040](DECISIONS.md#v040--callmediaplane--callstack-ownership-collapse); phase [cs](PHASES.md#cs--callstack-ownership-collapse-callmediaplane) |
| **V041 Lifecycle ports** | **Landed (ci0–ci3)** — `CallLifecycleSignalingPorts`; Stack composition root; CALLS.md table — [DECISIONS V041](DECISIONS.md#v041--calllifecycle-signaling-ports--stack-composition-root); phase [ci](PHASES.md#ci--callstack-composition-independence-lifecycle-ports) |
| **V042 Direct media ports** | **Landed (dm)** — CSM `CallDirectMediaPorts`; no `CallMediaBridge*` — [DECISIONS V042](DECISIONS.md#v042--calldirectmediaports--csm-without-callmediabridge); phase [dm](PHASES.md#dm--csm-direct-media-ports-no-callmediabridge) |
| **V043 Seat + Lifecycle ports** | **Landed (sl)** — CSM ports only; no `CallLifecycle*` / `CallMediaSeat*` facets — [DECISIONS V043](DECISIONS.md#v043--callsessionlifecycleports--callmediaseatports); phase [sl](PHASES.md#sl--csm-seat--lifecycle-ports-no-sibling-facets) |
| **V044 CallSessionWorkflow** | **Landed (sw)** — durable session/roster in CSM-owned Workflow — [DECISIONS V044](DECISIONS.md#v044--callsessionworkflow-durable-sessionroster); phase [sw](PHASES.md#sw--callsessionworkflow-extract) |
| **V045 Workflow hygiene** | **Landed (wh)** — wire-before-commit Invite/Accept/Decline; Decline Ends local; query dedupe + Peek kick — [DECISIONS V045](DECISIONS.md#v045--callsessionworkflow-hygiene-wire-first--query-dedupe); phase [wh](PHASES.md#wh--callsessionworkflow-hygiene) |
| **V046 Topology independence** | **Landed (tp)** — state clusters; HostPorts; Lifecycle/Seat ports; `CallHopMigrateWorkflow` — [DECISIONS V046](DECISIONS.md#v046--calltopologycontroller-independence); phase [tp](PHASES.md#tp--calltopologycontroller-independence-v046) |
| **V047 HopMigrate no-friend** | **Landed (hm)** — Workflow owns race clusters + ports/Ops; Topology refs + TopologyOps; no friend — [DECISIONS V047](DECISIONS.md#v047--callhopmigrateworkflow-owns-clusters-no-friend); phase [hm](PHASES.md#hm--callhopmigrateworkflow-no-friend-v047) |
| a2/a3 media | Historical LAN WebRTC dogfood (a2–a3); **not** product path after m2 |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3 |
| Hop reachability | Program in [media-hop-reachability](../media-hop-reachability/) — **Amp mesh** (L1+; punch H009 planned); app `call_hop_addrs` **not** product |
| **Call phase projection** | **V052:** `LiveCall::Phase` projects call state + per-call media progress (V037 State+Status); intents on `CallSessionManager::Apply`; `media_cancel_gen` on `LiveCalls`; N025 from "a call is shown"; gtest `call_lifecycle_test` |
| **m1 mobile LAN voice** | Android ↔ Android 1:1 Opus on `/pp/realtime/1.0.0` — **dogfood OK 2026-08-02** |
| **V031 call chrome modes** | Expanded / Immersive / Minimized + gestures landed (people grid for group voice; minimize chip) |
| **V032 media QoS structure** | Host receive policy doc; hop A↑/A↓ token buckets + session/participant caps; per-`stream_id` Opus + jitter playout; path_pressure → Opus bps; SFU AEAD under call media key |
| **Call media health UI** | Quality bars + Fair/Poor/NoAudio; Call details **Path** = direct / punched / circuit / media_relay (**`media_relay` only when hop.attached** — 1:1 Amp `sfu_mode` alone is not relay); subtitle shows path when seat Live; NoAudio/SendingOnly overrides Connected claim |
| **1:1 NAT TX-only escalate** | After DirectConnected, if TX alive + RX=0 for ~4s on non-circuit path → force circuit Ensure + re-`BeginSession` (once per call) |
| **1:1 vs stale CallSfuAttach** | Accept→P2P bumps migrate gen + clears SoftMigrate; inbound `CallSfuAttach` ignored unless N≥3 / WaitForAttach / SoftMigrate; stale CompleteAttach without flight ownership aborts StartSfu (dogfood: brief hop audio → chrome “direct”) |
| **V034 libp2p video_lo** | H264 on same 1:1 duplex + SFU ch1; v2 frames; shared call media key (one encrypt / hop fan-out); hop never sheds audio for video; Immersive per-peer tiles |
| Video on libp2p | **In progress (lv)** — LAN 1:1 Camera on is the first dogfood bar |
| **V050 group topology** | **Landed (gt0–gt6)** — full invite roster + joined-count arming; initiator leave keeps the rest on the hop with one agreed next owner; `planned_hop` from the invite list; Accept hop report (reached / unreachable) with the one adjustment or joiner refusal at the third join; later joiners move the whole group once, only to a hop every member can use, guests following the owner. Hard lab Phases 13–15 across three NATs and two hops. Paid-hop ownership handover deferred (pricing P004) — [PHASES gt](PHASES.md#gt--group-call-topology-v050) |

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| **m2 teardown** | libdatachannel unlinked; `CallP2pSignalingBridge` deleted; libp2p-only 1:1 |

## m1 mobile LAN — dogfood claimed (2026-08-02)

**Devices:** moto g7 play (`ZY323QRNJ9`) + Samsung SM-T380 (`dc07955772d54e6c`), same Wi‑Fi; package `dev.pp_browser.app`.

**Path:** Invite-embedded MediaKey → N025 ephemeral listen → **both roles dial immediately** ([V049](DECISIONS.md#v049--simultaneous-dial-on-accept-cross-nat-open) / B31; Amp A026 elects) → hello/ack → `DirectConnected` / `InCall` → bidirectional Opus (AEAD under call media key).

**Matrix:**

- [x] Accept click → `Accepting` / `AcceptInvite` off Browser IO
- [x] N025 `WantEphemeralListen` + bound `/tcp/<nonzero>`
- [x] `DirectConnected` / `Call-media Connect ok` → `InCall`
- [x] Bidirectional voice (no connect banner; audible both ways)
- [x] Leave → `Idle` (process stays up)

Filter: `adb logcat -s pp-browser:W` — release emit floor promotes INFO→WARNING, so lifecycle / call-media / ephemeral-listen **info** traces still appear.

**Implementation notes (call-media):**

- **V049 / B31:** both offerer and answerer dial on Connect; `EnsureAssociation` re-attempts within the dial budget after backoff. Inbound still wins if it lands first (`keep_inbound`). Historical “answerer first / offerer 15 s grace” failed phone→Mac under stateful NAT.
- Capture enqueues frames; **host IO thread** owns Yamux read/write (async pump) — do not block `read`/`write` on a worker while IO delivers
- Yamux `WriteQueue` copies on enqueue; `ReadBuffer::consumePart` soft-fails bad offsets (see [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md))
- Keep Accept / MediaKey-send / Connect / Poll HTTP **off** Browser IO

## Still open

| Area | State |
|------|-------|
| **Cross-net B26–B30** | B26/B28/B29 H012/B31 landed; B30 client poll fail-fast + infra still open — [CROSS_NETWORK_B25_B31.md](CROSS_NETWORK_B25_B31.md) |
| **rd D3/D4** | **Automated gates** below (purpose IDs). Human OEM sample optional — never the only gate |
| Hop peerstore / circuit | media-hop **L1–L3** + loopback compose landed; **L3.5 multi-hop** later (transitive R1↛B) |
| **Transport session SMs (V033 / N026)** | **s2a + s3a + s3b** + circuit compose; **ConnectAsync landed**; inbound MediaKey wait cancelable (**landed**); leftovers: sync L4 façades for tests; optional s4 if Leave hangs — [SESSION_MACHINES.md](SESSION_MACHINES.md#remaining-work-call-media--peer-honesty) |
| **Answerer MediaKey wait** | Exhaustion → `ConnectFailed` + `call.error.media_key_timeout` (no stuck MediaPending); KeyReady kick + **timeout compose** (`SetMediaKeyInboxPollRoundsForTest(0)`) |
| **Remote Leave / CallEnded chrome** | `EndCallLocal` closes the LiveCall — the projection goes Idle on either side's Leave (dual-stack both directions) |
| **Inbound Decline clears offerer** | `HandleInboundDecline` `EndCallLocal` when no remote Joined/Ringing/Invited remain (1:1); keeps call if another invitee still rings; dual-stack Decline wire (gtest) |
| **Outbound unanswered TTL** | `CallSessionLogic::ShouldAutoLeaveOutboundUnanswered` + `SweepExpiredInvites` LeaveCall (feature, not GUI-only); CallController Tick sweeps — gtest logic + compose |
| **Incoming invite expire** | `SweepExpiredInvites` Missed + `EndCallLocal` → `RemoteEnded` clears Ringing Idle / listen (CALLS expire → Idle; gtest) |
| **Retry after ConnectFailed** | Lifecycle `RetryClicked` re-arms `DirectConnecting` before `RetryP2pMedia`/`BeginSession` (gtest `RetryClickedRearms*` / `RetryP2pMediaAfterConnectFailed`) |
| **lv video** | Prefer loopback/probe; OEM dogfood only for Camera/HW encode |
| Group SoftMigrate in lifecycle | Phase hook reserved; not v1 (group topology rules: V050 / phase gt) |
| N≥3 unify engine on libp2p send/recv | N021 follow-on |

### rd automated exit (V038) — prefer over device dogfood

Doctrine: [TESTING.md](../../docs/architecture/TESTING.md) (promote downward); inventory [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) `B-CALL-*` / `B-HARD-CALL`.

| Gate | Purpose / evidence | Status |
|------|-------------------|--------|
| **D2 policy** | Lifecycle + topology Status gates; [`CallTxOnlyEscalateLogic`](../../src/lib/calls/CallTxOnlyEscalateLogic.h) | **PASS** (gtest) |
| **D3 dial without mDNS** | [`CallListenAddrsLogic`](../../src/lib/calls/CallListenAddrsLogic.h) + invite encode round-trip; CSM fills invite/accept from provider | **PASS** (gtest) |
| **D3 direct duplex** | `B-CALL-DIRECT`: Bridge + Kick + CSM compose + CallUiBackend + dual-stack (Invite/InCall/Leave + K-cycle + conflict) + `pp_call_direct_smoke` / `pp_call_conflict_smoke` | **Improved** (in-process product wire + K-cycle + conflict); Amp smokes green |
| **D4 circuit duplex** | `B-CALL-HOP`: `AmpCircuitCallMediaComposeTest` + `pp_call_hop_smoke` | **PASS** loopback; smoke scaffold |
| **D4 forced NAT stand-in** | `B-HARD-CALL` / `--suite hard` (A↛B netns → circuit) | Scaffold / nightly — **replaces** “two NATed phones” as regression wall |
| **OEM sample** | Audio session / Android mic-speaker — `covered-above` for policy | Optional; m1 LAN mobile already claimed |

**Do not** block rd on a second human NAT pair when hard-lab + hop smoke are green. Promote any future dogfood bug into gtest/compose in the same change.

### OEM sample (optional — not the regression wall)

- [x] Android↔Android LAN voice (m1, 2026-08-02)
- [ ] Android↔desktop packaging sample if hard-lab does not exercise shipped desktop binary (record here if run)
- [ ] Skip dedicated “NAT pair dogfood” when `B-HARD-CALL` is green

## Next agent — start here

1. Keep **pm** / **rd** / **cs** green: unit + `call` / `call-hop` / `hard` / `conflict` purpose IDs.
2. Optional: `pp_call_hop_smoke` when Docker image ready; or `pp-call-probe` product invite wire (large).
3. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay` if group SoftMigrate blocked.

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | Removed in m2 — mesh media only |
| SoftMigrate invents NAT | Stack dialable? then quote |
| Put SoftMigrate relay-cap nudge in CSM | Topology `OnPeerMediaRelayCapLearned` + [`CallMediaPlannerSelectLogic`](../../src/lib/calls/CallMediaPlannerSelectLogic.h) |
| Invent N025 listen from `TopPendingInvite` on tick | Lifecycle `WantEphemeralListen` only |
| Duplicate listen desire on CallStack | Lifecycle sole desire; stack only syncs Hub N025 execution |
| Put Amp dial/relay/hop unique_ptrs on CallStack | **V040** — `CallMediaPlane` owns mesh-media siblings |
| Cache CSM/stores/seat/lifecycle on the plane (`LiveRefs`) | Stack `BindMediaProducts` + deps callbacks only; plane `BindBridge` takes args per call |
| Hold `CallSessionManager*` on CallLifecycle | **V041** — `CallLifecycleSignalingPorts` from Stack |
| Hold `CallMediaBridge*` on CallSessionManager | **V042** — `CallDirectMediaPorts` from Stack |
| Hold `CallLifecycle*` / `CallMediaSeat*` on CSM | **V043** — Lifecycle/Seat ports + `WireTopology*` for child only |
| Split one class across `.cpp` files for size | Helpers in same TU or a new type — [AGENTS.md](../../AGENTS.md#conventions); inbound arms merged into `CallSessionManager.cpp` |
| Re-inline relay/dial/hop/bridge into mega-`Wire` | Keep `Wire` mesh-only; follow [AGENTS.md](../../AGENTS.md#conventions) function-complexity convention |
| Full-shell `SyncLayout` for Accept chrome | `RemountCallChrome` into `#shell-call-*-mount` only |
| Host-wide inbound request SM / rewrite working call-media “while here” | V033 — targeted session SMs; [SESSION_MACHINES.md](SESSION_MACHINES.md) docs first |
| Move `CallLifecycle` phases into `integration/host` | Product SM stays in feature; transport SM in host |
| Always-mounted `data-if` + Dirty for Accept layer | Presence mount via `RemountCallChrome`; Dirty only for labels/pulse inside a mounted layer |
| Recreate `CallMediaBridge` on N025 sync | Only when `CallSessionManager*` changes |
| Call-media `read`/`write` from a non-IO worker while pump runs | `Libp2pHost::Post` async pump only |
| `BlockingRead` hello/ack on WorkerPool; trust peer FIN | Async hello + handshake deadline + Yamux `reset()` — [SESSION_MACHINES peer honesty](SESSION_MACHINES.md#peer-honesty-rule-stream-waits) |
| Hold a mutex across blocking stream read from capture | Enqueue + IO-thread write |
| Put Accept / Connect / PollInbox on Browser IO | Dedicated workers / hop off IO |
| 1:1 auto SoftMigrate to `media_relay` | Circuit only for undialable 1:1; SFU is N≥3 |
| Treat status-bar Direct / dialable as bidirectional audio | Dialable ≠ duplex; health path + RX frames decide; TX-only escalates via circuit |
| Arm Bridge and Topology together after Accept | **V037** Status arms one planner; Deciding bumps `media_cancel_gen` |
| Block rd / V038 on human NAT-pair dogfood | Guard with gtest + compose + `B-CALL-HOP` / `B-HARD-CALL`; OEM sample optional |
| Rewrite transport SM in same PR as planner Apply | V039 / V033 — one machine layer per PR |
| Put SoftMigrate side effects on Bridge | Hop `Apply(SoftMigrateRequested)` only |
| Reintroduce CallController PollPendingSfuAttach / PollP2pConnectHealth tick | V039 — Direct health timer + Hop attach-wait timer; chrome may heal once on media `failed` |

## Call state consolidation (2026-10)

A live call's state was kept in about ten places by seven owners (2026-10 map): `LiveCall` (call
state, media Status), `CallSessionManager` (accept in flight, activity line), the topology / hop
structs (flight, attach wait, guest reattach), `CallMediaBridge` (direct planner, connect failed),
`CallMediaSeat` (path, media state), `CallMediaCoordinator` (path), the engine (`connection_state`,
`connected_at_ms`), the `CallUiState` snapshot, `CallPresenter` latches and the stored row. The
status line read seven of them at two moments (the snapshot, and the engine live); "Reconnecting"
had five sources, "live" three, the path four. The bugs that came of it: a call read Connected
while its direct stream had ended and the hop was not attached; the call timer reset to 0:00 on
every reconnect.

**Target:** `LiveCall` owns, per call: call state and end reason; one media sub-state (Deciding /
Direct: Connecting, Live, TxOnly, Reconnecting / Hop: Waiting, Attaching, Live, Migrating,
Reattaching / Failed, with its details); `live_since_ms`; the accepting sub-state; the recovery
reason. The planners send it events instead of reporting progress beside their own phases; the
seat keeps only the exclusive lock (holder, epoch, attach ticket); the engine's connection state
becomes an input event. `CallUiState` publishes one copy; the presenter keeps only UI latches (ring
shown, a click the owner has not taken yet). Pure deciders stay pure (`Decide*PlannerPhase`,
`DecideInCallStatus` on one snapshot, `CallMediaStatusLogic`, `PlannerFor` / `Allows*`).

**Landed (2026-10-23):**

1. `LiveCalls::NoteDirectHandedToHop` — a 1:1 stream ended for a hop not attached yet waits for it
   (HopWaiting), instead of a synthetic "connected" that promoted it to HopLive.
2. `LiveCallMediaProgress::live_since_ms` — when the call first went live, kept through failures
   and reconnects: the call timer and "was connected" read it (the engine's per-path
   `connected_at_ms` and the presenter's latch are gone from the screen).
3. `CallRecovery` — one reason (Migrating / WaitingForHop / Reattaching / HopLost) from
   `CallTopologyController::Recovery()` replaces the four recovery flags the screen read;
   `RecoveryInFlight` is the old "awaiting SFU recovery".

**Next, in order:** fold `accept_in_flight_` into `LiveCallState::Accepting`; make the seat's
Live / Failed derive from `LiveCall` (drop the hand-paired `note_live` + `on_connected` writes);
planners emit events into `LiveCall` (drop `report_progress` beside `Apply`); feed the engine's
connection state in as an event (the screen stops reading the engine); one path value.

End state: [CALLS.md § Call state: one owner](../../docs/architecture/CALLS.md#call-state-one-owner).

## Extraction sequence (history)

Landed (behavior-preserving + who-picks fix):

1. **Topology extract** — `CallTopologyController` owns soft-migrate / attach / wait / eject / hop helpers.
2. **Libp2p media bridge** — `CallMediaBridge` owns schedule/dial/retry/stop-media + 1:1 connect-fail / Retry.
3. **Dispatch cleanup** — thin `ApplyInboundControl` → `CallSessionWorkflow::HandleInbound*`.
4. **Pure who-picks / wait / fan-out** — `SoftMigrateLogic`, `CallHopAttachLogic` + fakes (`IMediaRelayClient` / `IDialRegistry`).
5. **Tests** — `CallMediaTopology` N≥3-only; SoftMigrate / wait / fan-out / topology controller unit tests; `media_relay_service_test` loopback remains integration.
6. **m2 teardown** — removed `CallP2pSignalingBridge` + libdatachannel from build; wire-compat ignore for `call_sdp` / `call_ice`.
