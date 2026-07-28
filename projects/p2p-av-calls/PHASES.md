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

- [ ] `CallSession` / participant store on `profile.db` (V011); vault-backed media key slots
- [ ] Signaling events via direct E2E system `control_type` (V012): invite / accept / decline / leave / roster / media_key / ended
- [ ] Origin-thread system messages: `call_started` / `call_ended`
- [ ] Invite-only join; guests without group membership
- [ ] Hostless end on last leave; epoch coordinator = min identity (V002)
- [ ] Push: `call_wake` type + client fetch-then-ring (V006); extend push project ADR/contracts + relay emit rule
- [ ] Basic ring / in-call shell UI (no media yet or stub)
- [ ] Unit tests: coordinator selection, session state machine, invite expiry

## a2 — 1:1 voice media

- [ ] WebRTC library **spike ADR** (V013); prove Opus on LAN
- [ ] ICE P2P on LAN dogfood; SFU/TURN via seed when available
- [ ] Shared media key wrap over pairwise E2E; epoch 1
- [ ] Two-device voice call green path (document NAT vs LAN in CURRENT_STATE)

## a3 — 1:1 video (desktop + mobile)

- [ ] Capture + render path in SDL/RmlUi shell (platform camera permissions)
- [ ] Lock video codec preference (H264 vs VP8)
- [ ] Camera off by default on join (V009)
- [ ] Mobile Client ↔ desktop / mobile↔mobile via seed SFU

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
