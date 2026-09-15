# P2P A/V calls — current state

**Last updated:** 2026-09-14

**North star:** [NETWORKING.md](../../docs/architecture/NETWORKING.md) + **[V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)** — HTTP + libp2p only; call media on libp2p (voice-first). **m2 done:** libdatachannel removed from build; wire-compat `call_sdp`/`call_ice` ignored.

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md) (**Call lifecycle**). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md).

## Landed

| Area | State |
|------|-------|
| Project docs | a3 done; **a4 thin**; **V026** libp2p-only media |
| ADRs | V001–**V038** |
| **V035 SoftMigrate scope** | PreferLocal only for **LAN-confirmed Link**; Site/Wide → org seed; ignore stale CallSfuAttach/HopRefuse/**CallAccept** when another call is bound |
| **V036 MediaSeat** | **Phase 3 landed** — `CallDirectPath` / `CallHopPath` token façades; CSM signaling-only for duplex start/stop; Phase 1–2 bind/Live/attach flight retained — [DECISIONS V036](DECISIONS.md#v036--mediaseat--exclusive-media-epoch) |
| **V037 State+Status FSM** | `CallPhase` + `CallMediaStatus`; one planner armed per pair; `media_cancel_gen`; gates on ScheduleStart / CallSfuAttach / CompleteAttach — [DECISIONS V037](DECISIONS.md#v037--calllifecycle-state--status-one-planner-armed) |
| **V038 rewrite debt** | N=2 = direct → punch → **circuit** call-media; SoftMigrate / `media_relay` = **N≥3 only** — [DECISIONS V038](DECISIONS.md#v038--n2-circuit-for-nat-softmigrate-reserved-for-n3); phase [rd](PHASES.md#rd--amp-call-media-rewrite-debt-v038) |
| **N→planner select** | `CallMediaPlannerSelectLogic` + Topology `OnPeerMediaRelayCapLearned`; CSM Accept no longer owns SoftMigrate nudge trees |
| **V039 planner FSMs** | **pm0–pm4 landed** — Direct/Hop `Apply` + logic gtests; health/attach-wait SM timers; CALLS race homes → planner phases — [DECISIONS V039](DECISIONS.md#v039--call-directhop-planner-machines) |
| a2/a3 media | Historical LAN WebRTC dogfood (a2–a3); **not** product path after m2 |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3 |
| Hop reachability | Program in [media-hop-reachability](../media-hop-reachability/) — **Amp mesh** (L1+; punch H009 planned); app `call_hop_addrs` **not** product |
| **CallLifecycle orchestrator** | **V037 State+Status:** `CallPhase` + `CallMediaStatus`; one planner armed; `media_cancel_gen`; N025 from `WantEphemeralListen`; gtest `call_lifecycle_test` |
| **m1 mobile LAN voice** | Android ↔ Android 1:1 Opus on `/pp-browser/realtime/1.0.0` — **dogfood OK 2026-08-02** |
| **V031 call chrome modes** | Expanded / Immersive / Minimized + gestures landed (people grid for group voice; minimize chip) |
| **V032 media QoS structure** | Host receive policy doc; hop A↑/A↓ token buckets + session/participant caps; per-`stream_id` Opus + jitter playout; path_pressure → Opus bps; SFU AEAD under call media key |
| **Call media health UI** | Quality bars + Fair/Poor/NoAudio; Call details **Path** = direct / punched / circuit / media_relay (**`media_relay` only when hop.attached** — 1:1 Amp `sfu_mode` alone is not relay); subtitle shows path when seat Live; NoAudio/SendingOnly overrides Connected claim |
| **1:1 NAT TX-only escalate** | After DirectConnected, if TX alive + RX=0 for ~4s on non-circuit path → force circuit Ensure + re-`BeginSession` (once per call) |
| **1:1 vs stale CallSfuAttach** | Accept→P2P bumps migrate gen + clears SoftMigrate; inbound `CallSfuAttach` ignored unless N≥3 / WaitForAttach / SoftMigrate; stale CompleteAttach without flight ownership aborts StartSfu (dogfood: brief hop audio → chrome “direct”) |
| **V034 libp2p video_lo** | H264 on same 1:1 duplex + SFU ch1; v2 frames; shared call media key (one encrypt / hop fan-out); hop never sheds audio for video; Immersive per-peer tiles |
| Video on libp2p | **In progress (lv)** — LAN 1:1 Camera on is the first dogfood bar |

## a4 thin in code (still relevant under V026)

| Area | State |
|------|-------|
| Topology | N≥3 → sticky initiator `RankMediaHops` → quote/attach → `call_sfu_attach` |
| Hop pick | Contacts ∪ org seed via `MeshHopPolicy`; PreferInCall; needs dialable **multiaddr** until L1 peerstore |
| Budgets / framing | N019 / N021 on SFU path |
| **m2 teardown** | libdatachannel unlinked; `CallP2pSignalingBridge` deleted; libp2p-only 1:1 |

## m1 mobile LAN — dogfood claimed (2026-08-02)

**Devices:** moto g7 play (`ZY323QRNJ9`) + Samsung SM-T380 (`dc07955772d54e6c`), same Wi‑Fi; package `dev.pp_browser.app`.

**Path:** Invite-embedded MediaKey → N025 ephemeral listen → answerer reverse-dial (primary) / offerer dial after inbound grace if answerer dialable (asymmetric LAN) → hello/ack → `DirectConnected` / `InCall` → bidirectional Opus (AEAD under call media key).

**Matrix:**

- [x] Accept click → `Accepting` / `AcceptInvite` off Browser IO
- [x] N025 `WantEphemeralListen` + bound `/tcp/<nonzero>`
- [x] `DirectConnected` / `Call-media Connect ok` → `InCall`
- [x] Bidirectional voice (no connect banner; audible both ways)
- [x] Leave → `Idle` (process stays up)

Filter: `adb logcat -s pp-browser:W` — release emit floor promotes INFO→WARNING, so lifecycle / call-media / ephemeral-listen **info** traces still appear.

**Implementation notes (call-media):**

- Answerer reverse-dials first; offerer waits ~8s for inbound then falls back to dial if the answerer is reachable (asymmetric LAN) — still one `newStream` at a time (`keep_inbound` if the other side wins the race)
- Capture enqueues frames; **host IO thread** owns Yamux read/write (async pump) — do not block `read`/`write` on a worker while IO delivers
- Yamux `WriteQueue` copies on enqueue; `ReadBuffer::consumePart` soft-fails bad offsets (see [LIBP2P_UPSTREAM.md](../../docs/architecture/LIBP2P_UPSTREAM.md))
- Keep Accept / MediaKey-send / Connect / Poll HTTP **off** Browser IO

## Still open

| Area | State |
|------|-------|
| **rd D3/D4** | **Automated gates** below (purpose IDs). Human OEM sample optional — never the only gate |
| Hop peerstore / circuit | media-hop **L1–L3** + loopback compose landed; **L3.5 multi-hop** later (transitive R1↛B) |
| **Transport session SMs (V033 / N026)** | **s2a + s3a + s3b** + circuit compose; **ConnectAsync landed**; leftovers: inbound-handler stall contract, sync L4 façades for tests; optional s4 if Leave hangs — [SESSION_MACHINES.md](SESSION_MACHINES.md#remaining-work-call-media--peer-honesty) |
| **Answerer MediaKey wait** | Exhaustion → `ConnectFailed` + `call.error.media_key_timeout` (no stuck MediaPending) |
| **lv video** | Prefer loopback/probe; OEM dogfood only for Camera/HW encode |
| Group SoftMigrate in lifecycle | Phase hook reserved; not v1 |
| N≥3 unify engine on libp2p send/recv | N021 follow-on |

### rd automated exit (V038) — prefer over device dogfood

Doctrine: [TESTING.md](../../docs/architecture/TESTING.md) (promote downward); inventory [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) `B-CALL-*` / `B-HARD-CALL`.

| Gate | Purpose / evidence | Status |
|------|-------------------|--------|
| **D2 policy** | Lifecycle + topology Status gates; `CallTxOnlyEscalateLogic` | **PASS** (gtest) |
| **D3 dial without mDNS** | `CallListenAddrsLogic` + invite encode round-trip; CSM fills invite/accept from provider | **PASS** (gtest) |
| **D3 direct duplex** | `B-CALL-DIRECT`: Bridge answerer start + Kick logic gtests + `CallMediaDirectServiceTest` + `pp_call_direct_smoke` | **Improved** (ScheduleStart→StartSfu / MediaPending / HopLive gate); smoke scaffold for full Invite→Leave |
| **D4 circuit duplex** | `B-CALL-HOP`: `AmpCircuitCallMediaComposeTest` + `pp_call_hop_smoke` | **PASS** loopback; smoke scaffold |
| **D4 forced NAT stand-in** | `B-HARD-CALL` / `--suite hard` (A↛B netns → circuit) | Scaffold / nightly — **replaces** “two NATed phones” as regression wall |
| **OEM sample** | Audio session / Android mic-speaker — `covered-above` for policy | Optional; m1 LAN mobile already claimed |

**Do not** block rd on a second human NAT pair when hard-lab + hop smoke are green. Promote any future dogfood bug into gtest/compose in the same change.

### OEM sample (optional — not the regression wall)

- [x] Android↔Android LAN voice (m1, 2026-08-02)
- [ ] Android↔desktop packaging sample if hard-lab does not exercise shipped desktop binary (record here if run)
- [ ] Skip dedicated “NAT pair dogfood” when `B-HARD-CALL` is green

## Next agent — start here

1. Keep **pm** / **rd** green: unit + `call` / `call-hop` / `hard` purpose IDs.
2. Mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup); confirm seed `media_relay` if group SoftMigrate blocked.

## Agent traps

| Wrong | Right |
|-------|-------|
| Reintroduce `call_hop_addrs` / app ICE gather | H007 — reachability **in** libp2p |
| Extend libdatachannel for 1:1 | Removed in m2 — mesh media only |
| SoftMigrate invents NAT | Stack dialable? then quote |
| Put SoftMigrate relay-cap nudge in CSM | Topology `OnPeerMediaRelayCapLearned` + `CallMediaPlannerSelectLogic` |
| Invent N025 listen from `TopPendingInvite` on tick | Lifecycle `WantEphemeralListen` only |
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
