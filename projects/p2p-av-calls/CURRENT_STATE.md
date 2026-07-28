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
| UI | 1:1 Voice/Video header actions; ring Accept/Decline + pulse; compact in-call bar (mute, elapsed, meters, Leave) |
| Media stack | `base/media/CallMediaEngine` — libdatachannel + libopus + SDL audio (V014) |
| Media key wrap | Pairwise AEAD `WrapKeyB64` / `UnwrapKeyB64` (V015); sent on accept + rotate |
| Tests | Coordinator / state / invite expiry; store CRUD; SDP/ICE codec; wrap round-trip |

## In progress / dogfood

| Area | State |
|------|-------|
| Two-device LAN Opus green path | **LAN dogfood OK (2026-07-28)** — bidirectional voice heard (Win↔Linux); ICE host candidates; `disableAutoNegotiation`; answerer `onFrame` path. Still no STUN/TURN. Linux needs `libpulse-dev` + `libasound2-dev`. |
| Call chrome polish | Ringtone + mute + elapsed timer + compact in-call bar + speaking meters (a2 UI) |
| NAT / mobile | **Not claimed** — mesh seed SFU + mobile mic permissions (Android `RECORD_AUDIO` landed in dogfood; iOS usage string / audio session) still TODO |

## Not started (code)

| Area | State |
|------|-------|
| Seed SFU (`audio_relay` / `video_relay`) | Mesh capability sketched; not implemented |
| App-level frame AEAD under shared media key | Epoch key distributed; wire media still DTLS-SRTP (V014) |
| Group start / multi-invite UX | Manager supports invite list; header is 1:1-only for now |
| Missed/declined history hints | Deferred (v1.1) |
| Video (a3) | Capture/render + codec lock |
| Mobile mic / camera permissions | Android manifest `RECORD_AUDIO` exercised in dogfood; iOS plist / `AVAudioSession` still open — see [PLATFORMS § A/V](../../docs/architecture/PLATFORMS.md#av-media-sdl--calls) |

## Mesh dependency snapshot

| Mesh phase | Call relevance |
|------------|----------------|
| n1 done | Role / listen / bootstrap exist |
| np / nr / nu / n3 | Needed for honest NAT + dial SFU |
| n4 audio/video relay | SFU for mobile default path |

## Next agent — close a2 / start a3 prep

**Goal:** Mark a2 complete after any remaining Windows Accept UI flake; then video spike (a3) or seed SFU track.

1. ~~Two devices on same LAN: start voice call → accept → confirm bidirectional audio.~~ **Done (LAN).**
2. Optional: re-verify Android→desktop once Accept chrome is solid; confirm `RECORD_AUDIO` on all Android builds.
3. If ICE fails off-LAN, do not claim success — wait for STUN/TURN or seed SFU.
4. Mute / ringtone / compact bar are in; dogfood those on a second LAN pass if time.

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
| Remount shell (`RequestSyncLayout`) to show/hide call ring or in-call UI | Keep overlays in shell with `data-if`; update via `DirtyWindow` only ([WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md#dom-sync-dirtywindow-vs-synclayout)) |
| Remount shell (`RequestSyncLayout`) from background poll every tick | Remount only when real shell structure changes; poll reconciles call state without remount |
