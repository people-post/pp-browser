# P2P A/V calls — current state

**Last updated:** 2026-07-28

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a2 done**; **a3 planned** V016–V018) |
| ADRs | V001–V018 in [DECISIONS.md](DECISIONS.md) — **V014** stack; **V016** a3 slice; **V017** H264 **platform HW**; **V018** capture/render |
| Product model | Hybrid WebRTC media; mesh signaling; invite-only guests; hostless; shared media key; `call_wake` |
| Delivery track | Parallel media vs mesh SFU; LAN dogfood for a2/a3 (V010 / V016) |
| Persistence | `call_sessions` / `call_participants` / `pending_call_invites` / `call_media_keys` on `profile.db` (V011) |
| Signaling | Direct E2E `ChatPayload` system controls via `CallSessionManager` (V012) + **`call_sdp` / `call_ice`** |
| History | Origin-thread `call_started` / `call_ended` local system rows |
| Push | Opaque `call_wake` (P008) + client fetch-then-ring |
| UI | 1:1 Voice/Video start; ring Accept/Decline + pulse; conflict ring; compact in-call bar (mute, elapsed, meters, Leave) — **no video tiles / camera toggle yet** |
| Media stack | `base/media/CallMediaEngine` — libdatachannel + libopus + SDL audio (V014); **audio only** |
| Media key wrap | Pairwise AEAD `WrapKeyB64` / `UnwrapKeyB64` (V015); sent on accept + rotate |
| Tests | Coordinator / state / invite expiry; store CRUD; SDP/ICE codec; wrap round-trip; call chrome sync |

## a2 closed (LAN voice)

| Area | State |
|------|-------|
| Two-device LAN Opus | **OK (2026-07-28)** — bidirectional voice (Win↔Linux); ICE host candidates; no STUN/TURN |
| Call chrome polish | Ringtone + mute + elapsed + meters; conflict End & Accept / Ignore |
| NAT / mobile voice | **Not claimed** — seed SFU + full mobile bring-up still TODO |

## Not started (code) — a3 prep

| Area | State |
|------|-------|
| H264 encode/decode | **Locked** platform HW (V017): Win MF / macOS VideoToolbox / Android MediaCodec; Linux VA-API best-effort — **no soft-codec product fallback** |
| Video track + shell tiles | Capture/render path locked in V018; not implemented |
| Seed SFU (`audio_relay` / `video_relay`) | Mesh capability sketched; not implemented — **not a3 exit** |
| App-level frame AEAD under shared media key | Epoch key distributed; wire media still DTLS-SRTP (V014) |
| Group start / multi-invite UX | Manager supports invite list; header is 1:1-only for now |
| iOS A/V permissions / session | **Separate mobile-bring-up** (V016) — not a3 |
| Android camera | Optional in a3 dogfood; `CAMERA` + runtime when exercised |

## Mesh dependency snapshot

| Mesh phase | Call relevance |
|------------|----------------|
| n1 done | Role / listen / bootstrap exist |
| np / nr / nu / n3 | Needed for honest NAT + dial SFU |
| n4 audio/video relay | SFU for mobile default path |

## Next agent — a3 implement

**Goal:** Implement LAN 1:1 video per V016–V018. Codec path: **platform HW** (V017). No OpenH264 vendor as product default.

1. Add `IVideoCodec` + Win Media Foundation and/or macOS VideoToolbox first; Linux VA-API best-effort; fail video send clearly when no encoder.
2. Wire encode → `H264RtpPacketizer` / decode ← depacketizer beside Opus in `CallMediaEngine`.
3. SDL camera on Camera-toggle only (off by default); shell stage + PiP textures (V018); no shell remount for show/hide.
4. Two LAN devices on HW-capable hosts: enable camera both sides → confirm remote video.
5. Update CURRENT_STATE when LAN video green; leave NAT/SFU/iOS unclaimed; note Linux no-encoder cases.

**Do not:** claim mobile NAT success without seed SFU; fold iOS into a3 exit; ship OpenH264/FFmpeg as a3 default; ambient group Join; record/screen-share.

**Parallel:** mesh **nr** → … → seed SFU; separate **iOS mobile-bring-up** (mic plist, `AVAudioSession`, later camera usage).

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
| Pull full libwebrtc / OpenH264-as-default for a3 | libdatachannel + Opus + SDL + **platform HW H264** (V017) |
| Expect video encode on every Linux box | VA-API best-effort; no encoder → fail send loudly (V017) |
| Replace DTLS-SRTP with shared key alone in a2 | Shared key distributed; DTLS-SRTP on wire (V014) |
| Require Pulse/ALSA on Win/Mac/mobile | Only Linux needs those `-dev` packages ([BUILD.md](../../docs/ops/BUILD.md)) |
| Ship mobile voice without RECORD_AUDIO / NSMicrophoneUsageDescription | Platform permissions — iOS = separate bring-up ([PLATFORMS](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls)) |
| Remount shell (`RequestSyncLayout`) to show/hide call ring or in-call UI | Keep overlays in shell with `data-if`; update via `DirtyWindow` only ([WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md#dom-sync-dirtywindow-vs-synclayout)) |
| Remount shell for video tiles / every frame | Persistent GL texture + DirtyWindow; never remount for pixels (V018) |
| Claim a3 done when only SFU path works | a3 = LAN video; SFU is mesh-gated (V016) |
| Pick VP8 as a3 primary | H264 (V017) |
| Blit video after Present outside RmlUi layout | Layout-owned tiles (V018) |
