# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a2 done**; **a3 in progress** V016–V019) |
| ADRs | V001–V019 in [DECISIONS.md](DECISIONS.md) (V016 updated: iOS wiring in a3) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera on Camera toggle; `IVideoCodec` factory |
| UI | In-call **Camera** + stage/PiP; **GL tile blit** via `CallVideoTileRenderer` (V018) |
| Platform HW | **Win MF + macOS/iOS VT + Linux VA-API + Android MediaCodec (NDK)** implemented |
| iOS wiring | `NSMicrophoneUsageDescription` + `NSCameraUsageDescription`; `AVAudioSession` play-and-record; `UIBackgroundModes` `audio` |
| CMake | `SDL_CAMERA ON`; MF/`VideoToolbox`/`mediandk` linked; Android `RECORD_AUDIO` + `CAMERA` |

## a2 closed (LAN voice)

| Area | State |
|------|-------|
| Two-device LAN Opus | **OK (2026-07-28)** — Win↔Linux; host ICE |
| NAT / mobile voice | **Not claimed** |

## a3 progress (code)

| Area | State |
|------|-------|
| Unified Opus+H264 SDP | **Done** in `CallMediaEngine` |
| SDL camera + local preview frames | **Done** |
| Camera / mute content + roster | **Done** |
| Shell Camera + stage chrome | **Done** |
| GL persistent texture tiles (V018) | **Done** — `CallVideoTileRenderer` uploads RGBA to `#call-remote-tile` / `#call-local-tile` |
| Win MF / macOS VT / Android MediaCodec / Linux VA-API | **Code landed** — multi-host LAN dogfood pending |
| iOS plist + AVAudioSession | **Done** — device dogfood optional (wiring-only exit) |
| LAN bidirectional video dogfood | **Next** — Win/macOS/Linux/Android/iOS on real HW hosts |

## Next agent — LAN dogfood + claim green

1. Dogfood bidirectional video on LAN across Win/macOS/Linux (+ Android/iOS when devices available).
2. Update this file when green; leave NAT/SFU unclaimed. Document Linux hosts without usable HW encoder (accepted).

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera.

## Agent traps

| Wrong | Right |
|-------|-------|
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Remount shell for every video frame | DirtyWindow + persistent texture (V018) |
| Hide Camera on voice-started calls | Same in-call once connected (V019) |
| Defer iOS plist/session from a3 | iOS wiring is a3 exit (V016, 2026-07-30) |
