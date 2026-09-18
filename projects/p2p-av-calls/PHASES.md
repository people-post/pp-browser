# P2P A/V calls — phases

Ordering: docs → mesh alignment → signaling/ring → 1:1 voice → 1:1 video → group → polish → promote.

Mesh prerequisites (see [p2p-mesh PHASES](../p2p-mesh/PHASES.md)): **np → nr → nu → n3**, then seed **audio/video SFU** (n4 media caps, volunteer). **a1** overlaps mesh (V010); **a2** LAN dogfood OK; NAT’d mobile needs seed SFU.

## v0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md` + `AGENTS.md`
- [x] ADRs V001–V009 (hybrid stack, hostless min-id coordinator, rotate-on-leave, shared media key, invite-only session, `call_wake`, cap, seed SFU, start/camera defaults)
- [x] Cross-link p2p-mesh (calls as SFU consumer)

## a0 — Mesh / SFU prerequisites alignment

- [x] Document call dependency on nr/nu/n3 + seed SFU in p2p-mesh + call DESIGN (V010)
- [x] `pp-node` seed profile sketch: volunteer `audio_relay` / `video_relay` (DESIGN § Mesh alignment)
- [x] Confirm contact-first hop selection applies to media SFU (N014 → DESIGN)
- [x] Dogfood path defined: LAN direct ICE without SFU for a2 (V010)
- [x] ADRs V010–V013 (parallel track, profile.db, ChatPayload signaling, WebRTC spike deferral)

## a1 — Signaling + session + history + ring

- [x] `CallSession` / participant store on `profile.db` (V011); vault-backed media key slots
- [x] Signaling events via direct E2E system `control_type` (V012): invite / accept / decline / leave / roster / media_key / ended
- [x] Origin-thread system messages: `call_started` / `call_ended`
- [x] Invite-only join; guests without group membership
- [x] Hostless end on last leave; epoch coordinator = min identity (V002)
- [x] Push: `call_wake` type + client fetch-then-ring (V006); extend push project ADR/contracts + relay emit rule
- [x] Basic ring / in-call shell UI (no media yet or stub)
- [x] Unit tests: coordinator selection, session state machine, invite expiry

## a2 — 1:1 voice media

- [x] WebRTC library **spike ADR** ([V014](DECISIONS.md#v014--media-stack-libdatachannel--libopus--sdl)); code path for Opus on LAN
- [x] ICE P2P signaling (`call_sdp` / `call_ice`); host candidates for LAN dogfood (no STUN/TURN yet)
- [x] Shared media key wrap over pairwise E2E ([V015](DECISIONS.md#v015--pairwise-wrap-aad-for-call_media_key)); epoch 1 on accept + rotate
- [x] Document platform audio deps (Linux Pulse/ALSA; Win WASAPI; Mac CoreAudio; mobile permissions TODO) — [BUILD](../../docs/ops/BUILD.md) + [PLATFORMS](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls)
- [x] Two-device voice call green path (document NAT vs LAN in CURRENT_STATE) — **LAN dogfood OK 2026-07-28; NAT not claimed**
- [x] Light mute + ringtone + compact in-call chrome (a2 polish)

## a3 — 1:1 video (LAN; desktop + Android + iOS wiring)

Delivery slice: [V016](DECISIONS.md#v016--a3-delivery-slice-lan-video-mobile-wiring-included). Codec: [V017](DECISIONS.md#v017--video-codec-h264-via-platform-hw). Shell path: [V018](DECISIONS.md#v018--video-capture--render-path-in-sdl--rmlui-shell). Unified shape: [V019](DECISIONS.md#v019--unified-call-media-shape-voicevideo-entry-only).

- [x] Initial SDP always Opus + H264 m-lines (V019); mute/camera = content only; audio mandatory / video best-effort
- [x] Platform HW H264 backends behind `IVideoCodec` (Win MF / macOS+iOS VT / Android MediaCodec / Linux VA-API best-effort)
- [x] Capture + encode + RTP video track; decode + persistent GL texture tiles (V018); encode ~640×360 desktop / ~360×640 mobile after orientation @ 15–24 fps
- [x] Shell: unified in-call chrome + **Camera** on voice- and video-started calls; stage/PiP (V019); camera off on join (V009); compact icon mute/camera/leave
- [x] Mobile capture orientation (`CameraCaptureOrientation`: Android Camera2 sensor + display; iOS interface orientation) + tile letterbox
- [x] LAN dogfood: Android ↔ Windows / **Android ↔ macOS** / **Windows ↔ macOS** bidirectional; Android→Linux / Windows→Linux / **Mac→Linux** one-way video when Linux host has **no camera** (receive/display OK); iOS wiring done — **OK 2026-07-31**; iOS device dogfood optional
- [x] Document LAN video OK + Linux no-camera / no-encoder send limits; **do not** claim NAT / seed SFU
- [x] iOS mic / `AVAudioSession` / camera usage + background `audio` — **wiring done**; device dogfood optional
- [x] macOS Local Network usage string (`NSLocalNetworkUsageDescription`) for Sequoia LAN ICE — packaged Info.plist

**Deferred (mesh-gated, not a3 exit):** Mobile Client ↔ desktop / mobile↔mobile via seed SFU (V008 / n4); network-adaptive encode.  
**Accepted:** Linux video **send** needs camera + usable HW H264 encoder (V017); dogfood Linux was camera-less (receive OK). Voice must continue (V019).

## a4 — Group calls (≤8), guests, rotate-on-leave

Delivery: [V020](DECISIONS.md#v020--a4-requires-true-sfu-no-full-mesh-media)–[V024](DECISIONS.md#v024--adaptive-call-media-over-generic-relay-channels). Blind `media_relay` (mesh N018–N021). No full-mesh.

- [x] Mesh gate: volunteer **`media_relay`** on org `pp-node` + desktop (default on) — n4-media / N021 framing
- [x] Call consumer: N≥3 via forwarder; **1:1 stays P2P**; soft-migrate same `call_id`; re-pick (V021) — thin path
- [x] **V024 adaptation:** shared policy module for **1:1 P2P and SFU** (audio ≫ lo ≫ hi; producer first); backends differ; a4 ships single video layer
- [x] App-layer E2E under call media key on SFU path (V032; relay never holds keys)
- [x] **↑/↓** budgets + **quote/ceiling** when hop used; initiator pays (V022 / N019) — volunteer quote path + **V032 hop token-bucket enforce**
- [x] Hop pick: **contacts ∪ org seed** only (V023 / N020)
- [ ] Multi-invite; mid-call guest invite — API yes; chrome polish pending
- [x] Rotate media key on leave + overlapping epochs (V003) — existing a1 path
- [ ] In-call roster (mute / camera / speaking if cheap) — mute/camera roster exists; speaking pending; **Immersive people grid** (V031) landed for voice presence
- [x] Reuse a3 Opus + H264 HW — **no** new device codec matrix in a4
- [x] ICE-fail **1:1** stays P2P (V025) — timeout + Retry; N≥3 ICE-fail → SFU wired; no auto 1:1 SFU

## a5 — Cap, polish, reconnect

- [ ] Load-test; raise effective cap toward **16** or keep **8** with product copy
- [ ] Full **video_lo + video_hi** — **deferred** until libp2p video (V026 voice-first)
- [ ] Reconnect / “reconnecting…” after brief network loss
- [x] 1:1 connect timeout + Retry (legacy PC path)
- [ ] Missed/declined history hints optional
- [ ] Document desktop dead-process ring limitation

## m1 — Libp2p-only voice (V026)

North star: [NETWORKING.md](../../docs/architecture/NETWORKING.md), [V026](DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking), mesh [N022](../p2p-mesh/DECISIONS.md#n022--libp2p-investment-http-settle-preferred-chain-backup). Mobile LAN: [V027](DECISIONS.md#v027--mobile-call-scoped-listen-on-wi-fi) + mesh [N025](../p2p-mesh/DECISIONS.md#n025--mobile-call-scoped-listen-on-wi-fi-not-full-node) / [nm](../p2p-mesh/PHASES.md#nm--mobile-call-scoped-listen-n025).

- [x] 1:1 Opus over libp2p direct (LAN dialable PeerId+ma) — `CallMediaDirectService` + `CallMediaBridge`
- [x] 1:1 undialable → hop / circuit (explicit; not ICE Retry) — `TryEnsureCallMediaReachable` + protocol-scoped circuit hops
- [x] Loopback compose: circuit + call-media Opus (`CircuitCallMediaComposeTest`); circuit + media_relay fan-out (`CircuitMediaRelayComposeTest`)
- [x] Mobile callee on Wi‑Fi: ephemeral listen during foreground call (V027 / nm)
- [ ] N≥3 remains `media_relay`; unify engine on libp2p send/recv (N021)
- [x] App AEAD under call media key on media frames (direct 1:1 path + SFU V032)
- [x] Receiver per-stream playout + hop load admission / A↑A↓ enforce (V032)
- [x] Stop extending legacy WebRTC bridge; libp2p connect-fail UI hints via `PlatformUserHints`
- [x] Dogfood: Android ↔ Android bidirectional voice on Wi‑Fi (moto g7 play ↔ SM-T380) — **OK 2026-08-02**; see [CURRENT_STATE.md](CURRENT_STATE.md)

## m2 — Teardown WebRTC product path

- [x] Remove libdatachannel PeerConnection from call bring-up
- [x] Drop product use of `call_sdp` / `call_ice` (compat: ignore unknown)
- [x] Delete legacy `CallP2pSignalingBridge`; unlink libdatachannel from build
- [x] Update CALLS.md lifecycle diagrams to libp2p-only

## a6 — Promote contracts

- [ ] Wire / wake / media-key normative text → `docs/contracts/` (and push/mesh cross-links)
- [ ] Freeze ADRs as superseded-by docs where appropriate
- [ ] Update CURRENT_STATE / README status

## sm — Transport session machines (V033) — docs before code

Robustness refactor for long-lived host media sessions. Spec: [SESSION_MACHINES.md](SESSION_MACHINES.md). Mesh attach twin: [MEDIA_RELAY_ATTACH.md](../p2p-mesh/MEDIA_RELAY_ATTACH.md) (N026). **Do not start structural code until s1 open questions are frozen.**

- [x] s0 — Design docs + [V033](DECISIONS.md#v033--transport-session-machines-not-host-wide-inbound-sm) (+ mesh N026)
- [x] s1 — Freeze open questions in SESSION_MACHINES (mutex strand, blocking Connect, instant Failed→Idle, Detach-then-Connect)
- [x] s2a — `CallMediaDirectService` session phases/events; glare + Detach via phase; loopback tests (`CallMediaDirectServiceTest`)
- [x] s2c — Flag soup collapsed to phase (+ `connect_settled` waiter / `offerer_glare`); race homes noted in SESSION_MACHINES / CALLS.md
- [x] s3a — media-relay inbound attach SM (mesh N026)
- [x] s3b — media-relay client `AcceptAndAttach` SM + Detach abort
- [x] Circuit compose loopbacks — `CircuitCallMediaComposeTest` + `CircuitMediaRelayComposeTest`
- [ ] s4 — Optional circuit bridge SM; point CALLS.md critical races at phase homes

## rd — Amp call-media rewrite debt (V038)

Pay off post-V026/m2 migration so 1:1 Amp call-media is mature: frozen requirements, no SoftMigrate-for-NAT, V037 test coverage, dial matrix exit. Spec: [V038](DECISIONS.md#v038--n2-circuit-for-nat-softmigrate-reserved-for-n3).

- [x] D0 — V038 ADR + this phase; CURRENT_STATE next-agent → `rd`
- [x] D1 — DESIGN / CALLS / SESSION_MACHINES / CURRENT_STATE: circuit vs `media_relay`; ConnectAsync landed; `StartSfu` naming note
- [x] D2 — gtests: KickAnswerer Status gates; Direct* blocks hop StartSfu; TX-only circuit escalate
- [x] D3 — **Automated:** [`CallListenAddrsLogic`](../../src/domain/messaging/CallListenAddrsLogic.h) + Bridge answerer ScheduleStart/Kick gtests + `B-CALL-DIRECT` evidence; OEM sample optional
- [x] D4 — **Automated:** `AmpCircuitCallMediaComposeTest` + `B-CALL-HOP` / `B-HARD-CALL` as NAT stand-in; CALLS V038; s4 deferred; no required human NAT pair

**Non-goals:** `StartSfu` rename campaign; L3.5 multi-hop; SoftMigrate-for-1:1 reopen; s4 unless Leave hangs.  
**Dogfood:** never the only gate — [TESTING.md](../../docs/architecture/TESTING.md); purpose IDs in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md).

## pm — Call planner machines (V039)

Layered Apply FSMs under Lifecycle Status: Direct (`CallMediaBridge`) and Hop (`CallTopologyController`). Spec: [V039](DECISIONS.md#v039--call-directhop-planner-machines); [SESSION_MACHINES planner section](SESSION_MACHINES.md#planner-machines-v039). Transport SMs remain V033 — do not rewrite call-media duplex in the same PR as a planner strangler.

- [x] pm0 — V039 ADR + SESSION_MACHINES planner section + this phase; CURRENT_STATE next-agent → `pm`
- [x] pm1 — Direct `Apply` + [`CallDirectPlannerLogic`](../../src/domain/messaging/CallDirectPlannerLogic.h) + gtests; Schedule/Key/Connect/TX-only/Release through Apply
- [x] pm2 — Hop `Apply` + [`CallHopPlannerLogic`](../../src/domain/messaging/CallHopPlannerLogic.h); SoftMigrate-as-event; inbound attach Status gates
- [x] pm3 — SM-owned timers replace `PollMeshConnectHealth` / `PollPendingSfuAttach` primary path
- [x] pm4 — Lifecycle/CSM thin Accept media router; CALLS.md critical races → planner phases + epochs

**Non-goals:** `StartSfu` rename; SoftMigrate-for-1:1; host-wide inbound SM; Drive-by transport rewrites in planner PRs.  
**Exit:** purpose IDs `B-CALL-DIRECT` / `B-CALL-HOP` / `B-HARD-CALL` green; illegal planner sequences covered in gtest.

## lv — Video on libp2p (V034)

Voice-on-libp2p is green ([m1](#m1--libp2p-only-voice-v026)). Video reuses a3 capture/tiles and N021 channel 1; it does **not** revive WebRTC.

- [x] V034 ADR freeze — H264 video_lo; v2 call-media frames; SFU E2E; hop audio-priority drop
- [x] Frame crypto v2 (`channel` byte) + 128 KiB 1:1 cap; decrypt v1 as audio
- [x] 1:1 `SendMedia` / `on_media`; bridge no longer drops `channel_id==1`
- [x] Producer bitrate + `call_video_refresh` IDR
- [x] SFU encrypt all channels; hop/client queues never shed audio for video; SoftMigrate IDR
- [x] Per-stream H264 decode (cap 4) + Immersive per-peer tiles
- [x] Camera gating / health copy / HOST_RECEIVE_POLICY
- [ ] Device dogfood: Android↔Android LAN video; one desktop pair; N=3 hop with two cameras

## cs — CallStack ownership collapse (CallMediaPlane)

Thin `CallStack` to phase assembly; mesh-media siblings live under **`CallMediaPlane`**. Behavior-preserving move — [V040](DECISIONS.md#v040--callmediaplane--callstack-ownership-collapse).

- [x] cs0 — ADR + PHASES; remove duplicate `ephemeral_listen_desired_` (Lifecycle is sole desire)
- [x] cs1 — `CallMediaPlane` owns Amp transport / dial / relay / hop-reach / bridge / dial book; stack thin-forwards
- [x] cs2 — Trim redundant stack surface; promote ownership map into [CALLS.md](../../docs/architecture/CALLS.md)

**Non-goals:** Seat inside CSM; Hub N025 listen *execution*; wire/behavior changes.  
**Exit:** `call_ui_backend_stack_test` + `call_dual_stack_compose_test` + call lifecycle gtests green; CALLS.md names Amp + CallMediaPlane.

## ci — CallStack composition independence (Lifecycle ports)

Siblings stay independent; **CallStack** is the only graph knower — [V041](DECISIONS.md#v041--calllifecycle-signaling-ports--stack-composition-root).

- [x] ci0 — ADR + PHASES
- [x] ci1 — `CallLifecycleSignalingPorts`; drop `CallSessionManager*` from Lifecycle
- [x] ci2 — `CallStack::BindSeatTeardown` helper
- [x] ci3 — CALLS.md composition-root table

**Non-goals:** Seat inside CSM; CSM↔Bridge pointer campaign; CallProfileStores.  
**Exit:** lifecycle + stack compose gtests green; CALLS.md composition table.

## dm — CSM Direct media ports (no CallMediaBridge*)

Continue composition independence: CSM talks to Direct media only via **`CallDirectMediaPorts`** — [V042](DECISIONS.md#v042--calldirectmediaports--csm-without-callmediabridge).

- [x] dm0 — ADR + ports + `MakeCallDirectMediaPorts`; Stack `BindMediaProducts` installs; drop `SetCallMediaBridge`
- [x] dm1 — CALLS.md composition table + file map

**Non-goals:** Seat ports campaign; Topology rewrite.  
**Exit:** inbound compose + stack/dual-stack + lifecycle gtests green.

## sl — CSM Seat + Lifecycle ports (no sibling facets)

Drop standing `CallLifecycle*` / `CallMediaSeat*` on CSM — [V043](DECISIONS.md#v043--callsessionlifecycleports--callmediaseatports).

- [x] sl0 — ADR + AGENTS/CALLS anti same-class file-split note
- [x] sl1 — `CallSessionLifecyclePorts`; `SetLifecyclePorts` + `WireTopologyLifecycle`
- [x] sl2 — `CallMediaSeatPorts`; `SetMediaSeatPorts` + `WireTopologySeat`
- [x] sl3 — Stack install/clear; compose test; CALLS composition table

**Non-goals:** Host-adapter rewrite; peer reach book; chrome pass-through shrink; new same-class `.cpp` splits.  
**Exit:** inbound compose + stack/dual-stack + answerer gtests green.

## sw — CallSessionWorkflow extract

Durable session/roster out of CSM — [V044](DECISIONS.md#v044--callsessionworkflow-durable-sessionroster).

- [x] sw0 — ADR + `CallSessionWorkflow` + HostPorts + CSM `BindWorkflowHostPorts`
- [x] sw1 — Leave/End/Decline/Sweep/Abandon + inbound Leave/Decline/Ended
- [x] sw2 — Start/Invite/Accept + remaining inbound; thin CSM forwards

**Non-goals:** Second chrome SM; Topology/Bridge move; multi-`.cpp` Workflow split.  
**Exit:** inbound compose + stack/dual-stack + answerer gtests green.

## wh — CallSessionWorkflow hygiene

Post-V044 durable-path cleanup — [V045](DECISIONS.md#v045--callsessionworkflow-hygiene-wire-first--query-dedupe).

- [x] wh0 — ADR + wire-before-commit Invite/Accept/Decline; Decline `EndCallLocal`
- [x] wh1 — Workflow owns ActiveLocalCall/TopPending (+sweep); Peek kick; HostPorts null-guards
- [x] wh2 — CALLS.md note; compose gtests green

**Non-goals:** Peer-reach book merge; Host adapters.  
**Exit:** inbound compose + stack/dual-stack gtests green.

## tp — CallTopologyController independence (V046)

Hop SoftMigrate/attach composition cleanup — [V046](DECISIONS.md#v046--calltopologycontroller-independence).

- [x] tp0 — ADR + SoftMigrateFlight / AttachWait / InboundAttachGate / GuestSfuSession / PublisherStreams / SfuSurface clusters
- [x] tp1 — `CallTopologyHost` → HostPorts; CSM drops dual-inherit
- [x] tp2 — `CallTopologyLifecyclePorts` + `CallTopologySeatPorts`; Stack install
- [x] tp3 — `CallHopMigrateWorkflow` extract; CALLS promote

**Non-goals:** Peer dial-book merge; Bridge facet ports; same-class Topology `.cpp` splits.  
**Exit:** topology unit + inbound/dual-stack/ui-backend compose gtests green.

## hm — CallHopMigrateWorkflow no-friend (V047)

Drop Topology friend + private poke — [V047](DECISIONS.md#v047--callhopmigrateworkflow-owns-clusters-no-friend).

- [x] hm0 — ADR; Workflow owns SoftMigrateFlight / AttachWait / InboundAttachGate / GuestSfuSession / PublisherStreams / SfuSurface
- [x] hm1 — Host/Lifecycle/Seat ports + TopologyOps on Workflow; Topology refs + BindHopMigratePortsAndOps
- [x] hm2 — CALLS.md promote; topology + compose gtests green

**Non-goals:** Peer dial-book merge; Bridge facet ports.  
**Exit:** topology unit + inbound compose gtests green.

## ha — Hop arming vocabulary (V048 first application)

Apply the repo-wide [composition vocabulary](../../docs/architecture/COMPOSITION_VOCABULARY.md) guideline ([V048](DECISIONS.md#v048--composition-vocabulary-no-upward-concepts)) to Topology/Workflow first (Lifecycle enums must not appear downward).

- [x] ha0 — ADR (project guideline) + COMPOSITION_VOCABULARY.md + CALLS.md note
- [x] ha1 — Replace `CallTopologyLifecyclePorts` with hop-native arming / progress ports; Stack adapter maps to Lifecycle Status
- [x] ha2 — Drop `CallMediaStatus` / Lifecycle status reads from Topology + `CallHopMigrateWorkflow`; prefer single hop `Apply`/progress path
- [x] ha3 — Bridge Lifecycle pointer → `CallDirectArmingPorts` (same guideline)
- [x] ha4 — Port structs on consumer headers; `Make*` private on CallStack / CSM; delete free `*Ports.{h,cpp}`
- [x] ha5 — Workflow owns migrate arming/seat ports; Topology projects (no Topology include in Workflow)
- [x] ha6 — Bridge Direct seat ports; Workflow migrate host ports; SessionWorkflow HostPorts vocabulary trim

**Non-goals:** Ownership-tree change (Topology under Lifecycle); moving SoftMigrate races into Lifecycle; rewriting every non-calls debt site in this phase; full SessionWorkflow HostPorts cluster split.  
**Exit:** topology unit + inbound/dual-stack compose gtests green; Topology/Workflow headers free of `CallLifecycleTypes` Status writers.

## Later horizons

- [ ] `video_hi` / simulcast
- [ ] Free device rotation on mobile
- [ ] CallKit / ConnectionService-class OS call UI
- [ ] Screen share
- [ ] Recording (explicit user action)
- [ ] Ambient group Join policy (if ever)
- [ ] Paid SFU metering as **capacity regulation** (N020 mid/long) — not revenue-first
