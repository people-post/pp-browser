# P2P A/V calls — current state

**Last updated:** 2026-07-28

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 + **a0** + **a1** + **a2 in progress**) |
| ADRs | V001–V015 in [DECISIONS.md](DECISIONS.md) — **V014** locks libdatachannel + Opus + SDL |
| Product model | Hybrid WebRTC media; mesh signaling; invite-only guests; hostless; shared media key; `call_wake` |
| Delivery track | Parallel a1 vs mesh SFU; LAN dogfood for early a2 (V010) |
| Persistence | `call_sessions` / `call_participants` / `pending_call_invites` / `call_media_keys` on `profile.db` (V011) |
| Signaling | Direct E2E `ChatPayload` system controls via `CallSessionManager` (V012) + **`call_sdp` / `call_ice`** |
| History | Origin-thread `call_started` / `call_ended` local system rows |
| Push | Opaque `call_wake` (P008) + client fetch-then-ring |
| UI | 1:1 Voice/Video header actions; ring Accept/Decline; in-call Leave; subtitle from media state |
| Media stack | `base/media/CallMediaEngine` — libdatachannel + libopus + SDL audio (V014) |
| Media key wrap | Pairwise AEAD `WrapKeyB64` / `UnwrapKeyB64` (V015); sent on accept + rotate |
| Tests | Coordinator / state / invite expiry; store CRUD; SDP/ICE codec; wrap round-trip |

## In progress / dogfood

| Area | State |
|------|-------|
| Two-device LAN Opus green path | **Code ready** — needs human LAN dogfood (two desktops, same network). No STUN/TURN yet. Linux: `libpulse-dev` + `libasound2-dev`. Win/Mac: WASAPI/CoreAudio (no extra packages). See [BUILD.md](../../docs/ops/BUILD.md) + [PLATFORMS § A/V](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls). |
| NAT / mobile | **Not claimed** — mesh seed SFU + mobile mic permissions (Android `RECORD_AUDIO`, iOS usage string / audio session) still TODO |

## Not started (code)

| Area | State |
|------|-------|
| Seed SFU (`audio_relay` / `video_relay`) | Mesh capability sketched; not implemented |
| App-level frame AEAD under shared media key | Epoch key distributed; wire media still DTLS-SRTP (V014) |
| Group start / multi-invite UX | Manager supports invite list; header is 1:1-only for now |
| Missed/declined history hints | Deferred (v1.1) |
| Video (a3) | Capture/render + codec lock |
| Mobile mic / camera permissions | Android manifest + iOS plist / `AVAudioSession` — see [PLATFORMS § A/V](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls) |

## Mesh dependency snapshot

| Mesh phase | Call relevance |
|------------|----------------|
| n1 done | Role / listen / bootstrap exist |
| np / nr / nu / n3 | Needed for honest NAT + dial SFU |
| n4 audio/video relay | SFU for mobile default path |

## Next agent — finish a2 dogfood + polish

**Goal:** Prove 1:1 Opus on LAN; mark a2 done. No claim of NAT’d mobile until seed SFU.

1. Two devices on same LAN: start voice call → accept → confirm bidirectional audio.
2. Document result (LAN OK / failure notes) in this file; check remaining [PHASES.md](PHASES.md) a2 boxes.
3. If ICE fails on LAN, inspect `call_sdp`/`call_ice` delivery latency and host candidates.
4. Optional: light reconnect / mute plumbing before a3.

**Do not:** claim mobile NAT success without seed SFU; ambient group Join; record/screen-share.

**Parallel mesh work (other agents):** `pp-node` (**np done**) → reachability/UPnP (**nr/nu**) → circuit → seed SFU — required before NAT’d mobile green path.

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
| Pull full libwebrtc for a2 | libdatachannel + Opus + SDL (V014) |
| Replace DTLS-SRTP with shared key alone in a2 | Shared key distributed; DTLS-SRTP on wire (V014) |
| Require Pulse/ALSA on Win/Mac/mobile | Only Linux needs those `-dev` packages ([BUILD.md](../../docs/ops/BUILD.md)) |
| Ship mobile voice without RECORD_AUDIO / NSMicrophoneUsageDescription | Add platform permissions first ([PLATFORMS](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls)) |
| Remount shell (`RequestSyncLayout`) from background poll every tick | Remount only when call ring / in-call **layers** change; poll reconciles; labels use `DirtyWindow` ([WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md#dom-sync-dirtywindow-vs-synclayout)) |
