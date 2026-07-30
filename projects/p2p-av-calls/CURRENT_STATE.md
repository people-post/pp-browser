# P2P A/V calls — current state

**Last updated:** 2026-07-29

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a2 done**; **a3 in progress** V016–V019) |
| ADRs | V001–V019 in [DECISIONS.md](DECISIONS.md) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera on Camera toggle; `IVideoCodec` factory |
| UI | In-call **Camera** + stage/PiP placeholders (`data-if`); roster fan-out for mute/camera |
| Platform HW | **Win MF + macOS VT implemented** (unverified here — no Win/Apple SDK on this host); Linux/Android still unavailable stubs |
| CMake | `SDL_CAMERA ON`; MF/`VideoToolbox` linked on Win/Apple; Android `RECORD_AUDIO` + `CAMERA` |

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
| Win MF / macOS VT / Android MediaCodec / Linux VA-API | **Win + macOS code landed** (dogfood next); Android/Linux still stub |
| LAN bidirectional video dogfood | **Next** — Win/macOS HW code ready to try; needs real-host dogfood + GL tile blit |

## Next agent — finish a3 codecs + tile blit

1. Dogfood Win MF + macOS VideoToolbox on real HW hosts (LAN bidirectional). Android MediaCodec + Linux VA-API still stubs.
2. Wire persistent GL texture upload into `#call-remote-tile` / `#call-local-tile` (V018) — frames already in `CopyLocalVideoFrame` / `CopyRemoteVideoFrame`.
3. Update this file when green; leave NAT/SFU/iOS unclaimed.

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera; fold iOS into a3 exit.

## Agent traps

| Wrong | Right |
|-------|--------|
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Remount shell for every video frame | DirtyWindow + persistent texture (V018) |
| Hide Camera on voice-started calls | Same in-call once connected (V019) |
