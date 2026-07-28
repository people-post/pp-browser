# P2P A/V calls — current state

**Last updated:** 2026-07-28

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 + **a0** + **a1**) |
| ADRs | V001–V013 in [DECISIONS.md](DECISIONS.md) |
| Product model | Hybrid WebRTC media; mesh signaling; invite-only guests; hostless; shared media key; `call_wake` |
| Delivery track | Parallel a1 vs mesh SFU; LAN dogfood for early a2 (V010) |
| Persistence | `call_sessions` / `call_participants` / `pending_call_invites` / `call_media_keys` on `profile.db` (V011) |
| Signaling | Direct E2E `ChatPayload` system controls via `CallSessionManager` (V012) |
| History | Origin-thread `call_started` / `call_ended` local system rows |
| Push | Opaque `call_wake` (P008) + client fetch-then-ring |
| UI | 1:1 Voice/Video header actions; ring Accept/Decline; in-call Leave stub |
| Tests | Coordinator / state transitions / invite expiry; store CRUD |

## Not started (code)

| Area | State |
|------|-------|
| WebRTC / media integration | Absent (library spike at a2) |
| Pairwise AEAD wrap for media keys | Stub (`StubWrapKeyB64`) until a2 |
| Seed SFU (`audio_relay` / `video_relay`) | Mesh capability sketched; not implemented |
| Group start / multi-invite UX | Manager supports invite list; header is 1:1-only for now |
| Missed/declined history hints | Deferred (v1.1) |

## Mesh dependency snapshot

| Mesh phase | Call relevance |
|------------|----------------|
| n1 done | Role / listen / bootstrap exist |
| np / nr / nu / n3 | Needed for honest NAT + dial SFU |
| n4 audio/video relay | SFU for mobile default path |

## Next agent — start here (a2)

**Goal:** 1:1 voice media on LAN dogfood; WebRTC spike ADR (V013). No claim of NAT’d mobile until seed SFU.

1. Read [DESIGN.md](DESIGN.md) media plane + ADRs **V001, V004, V010, V013**.
2. Spike WebRTC library choice → short ADR; prove Opus 1:1 on LAN.
3. Wire ICE candidates into signaling `detail` (or follow-up controls); consume shared media key (replace stub wrap).
4. Prefer direct ICE on LAN; SFU path when mesh seed available.
5. Two-device voice green path; document LAN vs NAT in this file.
6. Update [PHASES.md](PHASES.md) a2 checkboxes in the same PR.

**Do not:** claim mobile NAT success without seed SFU; ambient group Join; record/screen-share.

**Parallel mesh work (other agents):** `pp-node` (np) → reachability/UPnP → circuit → seed SFU — required before NAT’d mobile green path.

## Agent traps

| Wrong | Right |
|-------|--------|
| Reuse group N-ciphertext for media frames | Shared call media key (V004) |
| Ambient Join for whole group roster | Invite-only (V005) |
| Creator is permanent crypto host | Min-identity coordinator among remaining (V002) |
| Custom Opus-over-libp2p as product media | WebRTC-shaped + mesh SFU (V001) |
| Put `call_id` in FCM body | Opaque `call_wake` (V006) |
| Mobile hosts SFU | Client consumes seed/friend SFU (V008) |
| Block a1 on mesh SFU | Parallel track (V010) |
| Store call roster only in thread.db | profile.db (V011) |
