# P2P A/V calls — current state

**Last updated:** 2026-07-28

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 + **a0**) |
| ADRs | V001–V013 in [DECISIONS.md](DECISIONS.md) |
| Product model | Hybrid WebRTC media; mesh signaling; invite-only guests; hostless; shared media key; `call_wake` |
| Delivery track | Parallel a1 vs mesh SFU; LAN dogfood for early a2 (V010) |
| Persistence / signaling | profile.db + vault keys (V011); ChatPayload system controls (V012) |

## Not started (code)

| Area | State |
|------|-------|
| CallSessionManager / signaling | Absent |
| WebRTC / media integration | Absent (library spike at a2) |
| `call_wake` push type | Absent (`inbox_wake` only today) |
| Call UI | Absent |
| Seed SFU (`audio_relay` / `video_relay`) | Mesh capability sketched; not implemented |

## Mesh dependency snapshot

| Mesh phase | Call relevance |
|------------|----------------|
| n1 done | Role / listen / bootstrap exist |
| np / nr / nu / n3 | Needed for honest NAT + dial SFU |
| n4 audio/video relay | SFU for mobile default path |

## Next agent — start here (a1)

**Goal:** Call session + pairwise signaling + history hints + ring shell — **no WebRTC yet**.

1. Read [DESIGN.md](DESIGN.md) (entities, lifecycle, signaling table) and ADRs **V002, V005, V006, V011, V012**.
2. Add `profile.db` tables / store API for `call_sessions` + `call_participants` (+ pending invites); vault hook for media key bytes (can stub key wrap until crypto wiring).
3. Define `control_type` strings + `detail` JSON; send/receive via existing direct E2E / outbox path (mirror group system-message pattern where possible).
4. On start/end: append `call_started` / `call_ended` to origin thread when linked.
5. Extend push: `call_wake` in contracts + client “fetch invites → ring”; coordinate relay emit (opaque).
6. Minimal UI: start call from 1:1 header, incoming accept/decline, leave; in-call chrome can be stub.
7. Tests: min-identity coordinator; state transitions; invite expiry.
8. Update this file + [PHASES.md](PHASES.md) checkboxes in the same PR.

**Do not:** vendor WebRTC in a1; claim NAT’d mobile calls; ambient group Join; group N-ciphertext for signaling.

**Parallel mesh work (other agents):** `pp-node` (np) → reachability/UPnP → circuit → seed SFU — required before a2 mobile green path.

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
