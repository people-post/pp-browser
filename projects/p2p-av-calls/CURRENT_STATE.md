# P2P A/V calls — current state

**Last updated:** 2026-07-29

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a2 done**; **a3 in progress** V016–V019) |
| ADRs | V001–V019 in [DECISIONS.md](DECISIONS.md) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera on Camera toggle; `IVideoCodec` factory |
| UI | In-call **Camera** + stage/PiP placeholders (`data-if`); roster fan-out for mute/camera |
| Platform HW | **Win MF + macOS VT + Linux VA-API (libva DRM) implemented**; Android still unavailable stub. Win/macOS dogfood pending on real hosts; Linux encode/decode smoke OK on radeonsi (2026-07-29) |
| CMake | `SDL_CAMERA ON`; MF/`VideoToolbox` linked on Win/Apple; Linux soft-links `libva`/`libva-drm` (`PP_BROWSER_HAS_LIBVA`); Android `RECORD_AUDIO` + `CAMERA` |

## a2 closed (LAN voice)

| Area | State |
|------|-------|
| Two-device LAN Opus | **OK (2026-07-28)** — Win↔Linux; host ICE |
| NAT / mobile voice | **Not claimed** |

## a3 progress (code)

| Area | State |
|------|-------|
| Unified Opus+H264 SDP | **Done** in `CallMediaEngine` |
| SDL camera + local preview frames | **Done** (RGBA queue for tiles; GL blit still TODO) |
| Camera / mute content + roster | **Done** (`SetLocalVideoEnabled` / `SetLocalAudioMuted`) |
| Shell Camera + stage chrome | **Done** (placeholders; persistent GL texture upload **not yet**) |
| Win MF / macOS VT / Android MediaCodec / Linux VA-API | **Win + macOS + Linux code landed**; Android still stub. Linux: decoder-only hosts OK; no encoder → video send fails, voice continues (V017/V019) |
| LAN bidirectional video dogfood | **Next** — Win/macOS/Linux HW ready to try; needs multi-host dogfood + GL tile blit |

## Next agent — finish a3 codecs + tile blit

1. Dogfood Win MF + macOS VideoToolbox + Linux VA-API on real HW hosts (LAN bidirectional). Android MediaCodec still stub.
2. Wire persistent GL texture upload into `#call-remote-tile` / `#call-local-tile` (V018) — frames already in `CopyLocalVideoFrame` / `CopyRemoteVideoFrame`.
3. Update this file when green; leave NAT/SFU/iOS unclaimed. Document Linux hosts without usable HW encoder (accepted).

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera; fold iOS into a3 exit.

## Agent traps

| Wrong | Right |
|-------|--------|
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Remount shell for every video frame | DirtyWindow + persistent texture (V018) |
| Hide Camera on voice-started calls | Same in-call once connected (V019) |
