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

## a3 — 1:1 video (LAN; desktop + Android)

Delivery slice: [V016](DECISIONS.md#v016--a3-delivery-slice-lan-video-first-sfu--ios-separate). Codec: [V017](DECISIONS.md#v017--video-codec-h264-via-platform-hw). Shell path: [V018](DECISIONS.md#v018--video-capture--render-path-in-sdl--rmlui-shell). Unified shape: [V019](DECISIONS.md#v019--unified-call-media-shape-voicevideo-entry-only).

- [x] Initial SDP always Opus + H264 m-lines (V019); mute/camera = content only; audio mandatory / video best-effort
- [ ] Platform HW H264 backends behind `IVideoCodec` (Win MF / macOS VideoToolbox primary; Android MediaCodec in a3; Linux VA-API best-effort) — **Win + macOS + Linux VA-API landed; Android stub; multi-host dogfood pending**
- [ ] Capture + encode + RTP video track; decode + persistent GL texture tiles (V018); encode ~640×360 @ 15–24 fps — capture/RTP/local preview + HW encode path done; GL blit pending
- [x] Shell: unified in-call chrome + **Camera** on voice- and video-started calls; stage/PiP placeholders (V019); camera off on join (V009) — pixel blit pending
- [ ] LAN dogfood: Win/macOS primary + Android; bidirectional video when enabled
- [ ] Document LAN video OK + Linux “no encoder” limitation; **do not** claim NAT / seed SFU
- [ ] iOS mic / `AVAudioSession` / camera usage — **not a3**; separate mobile-bring-up task

**Deferred (mesh-gated, not a3 exit):** Mobile Client ↔ desktop / mobile↔mobile via seed SFU (V008 / n4); network-adaptive encode.  
**Accepted:** Linux hosts without usable HW H264 encoder may fail video send (V017); voice must continue (V019).

## a4 — Group calls (≤8), guests, rotate-on-leave

- [ ] SFU topology for N≥3; enforce engineering cap **8**
- [ ] Multi-invite; mid-call guest invite
- [ ] Rotate media key on leave + overlapping epochs (V003)
- [ ] In-call roster (mute / camera / speaking if cheap)

## a5 — Cap, polish, reconnect

- [ ] Load-test; raise effective cap toward **16** or keep **8** with product copy
- [ ] Reconnect / “reconnecting…” after brief network loss
- [ ] Missed/declined history hints optional
- [ ] Document desktop dead-process ring limitation

## a6 — Promote contracts

- [ ] Wire / wake / media-key normative text → `docs/contracts/` (and push/mesh cross-links)
- [ ] Freeze ADRs as superseded-by docs where appropriate
- [ ] Update CURRENT_STATE / README status

## Later horizons

- [ ] CallKit / ConnectionService-class OS call UI
- [ ] Screen share
- [ ] Recording (explicit user action)
- [ ] Ambient group Join policy (if ever)
- [ ] Paid SFU metering (mesh N010) when volunteer capacity is insufficient
