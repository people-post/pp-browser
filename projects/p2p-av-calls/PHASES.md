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
- [ ] App-layer E2E under call media key (relay never holds keys) — follow-on
- [x] **↑/↓** budgets + **quote/ceiling** when hop used; initiator pays (V022 / N019) — volunteer quote path
- [x] Hop pick: **contacts ∪ org seed** only (V023 / N020)
- [ ] Multi-invite; mid-call guest invite — API yes; chrome polish pending
- [x] Rotate media key on leave + overlapping epochs (V003) — existing a1 path
- [ ] In-call roster (mute / camera / speaking if cheap) — mute/camera roster exists; speaking pending
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

- [x] 1:1 Opus over libp2p direct (LAN dialable PeerId+ma) — `CallMediaDirectService` + `CallLibp2pMediaBridge`
- [x] 1:1 undialable → hop / circuit (explicit; not ICE Retry) — `TryEnsureCallMediaReachable` + protocol-scoped circuit hops
- [x] Mobile callee on Wi‑Fi: ephemeral listen during foreground call (V027 / nm)
- [ ] N≥3 remains `media_relay`; unify engine on libp2p send/recv (N021)
- [x] App AEAD under call media key on media frames (direct 1:1 path)
- [x] Stop extending legacy WebRTC bridge; libp2p connect-fail UI hints via `PlatformUserHints`
- [x] Dogfood: Android ↔ Android bidirectional voice on Wi‑Fi (moto g7 play ↔ SM-T380) — **OK 2026-08-02**; see [CURRENT_STATE.md](CURRENT_STATE.md)
- [ ] Dogfood: Android ↔ desktop voice without WebRTC

## m2 — Teardown WebRTC product path

- [x] Remove libdatachannel PeerConnection from call bring-up
- [x] Drop product use of `call_sdp` / `call_ice` (compat: ignore unknown)
- [x] Delete legacy `CallP2pSignalingBridge`; unlink libdatachannel from build
- [x] Update CALLS.md lifecycle diagrams to libp2p-only

## a6 — Promote contracts

- [ ] Wire / wake / media-key normative text → `docs/contracts/` (and push/mesh cross-links)
- [ ] Freeze ADRs as superseded-by docs where appropriate
- [ ] Update CURRENT_STATE / README status

## Later horizons

- [ ] Video on libp2p (after voice green)
- [ ] Free device rotation on mobile
- [ ] CallKit / ConnectionService-class OS call UI
- [ ] Screen share
- [ ] Recording (explicit user action)
- [ ] Ambient group Join policy (if ever)
- [ ] Paid SFU metering as **capacity regulation** (N020 mid/long) — not revenue-first
