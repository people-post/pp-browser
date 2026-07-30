# P2P A/V calls — current state

**Last updated:** 2026-07-29

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a2 done**; **a3 in progress** V016–V019) |
| ADRs | V001–V019 in [DECISIONS.md](DECISIONS.md) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera on Camera toggle; `IVideoCodec` factory |
| UI | In-call **Camera** + stage/PiP placeholders (`data-if`); roster fan-out for mute/camera |
| Platform HW | **Stubs** — Linux/Win/macOS/Android return unavailable until MF/VT/MediaCodec/VA-API wired; preview may work without encode |
| CMake | `SDL_CAMERA ON`; Android `RECORD_AUDIO` + `CAMERA` in manifest |

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
| Win MF / macOS VT / Android MediaCodec / Linux VA-API | **Stub** → unavailable (encode fails soft; voice continues) |
| LAN bidirectional video dogfood | **Blocked** on platform HW encode+decode |

## Next agent — finish a3 codecs + tile blit

1. Implement real `IVideoCodec` backends: Win Media Foundation, macOS VideoToolbox first; Android MediaCodec; Linux VA-API best-effort.
2. Wire persistent GL texture upload into `#call-remote-tile` / `#call-local-tile` (V018) — frames already in `CopyLocalVideoFrame` / `CopyRemoteVideoFrame`.
3. LAN dogfood Win/macOS (+ Android): Camera on both sides → remote video.
4. Update this file when green; leave NAT/SFU/iOS unclaimed.

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera; fold iOS into a3 exit.

## Agent traps

| Wrong | Right |
|-------|--------|
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Remount shell for every video frame | DirtyWindow + persistent texture (V018) |
| Hide Camera on voice-started calls | Same in-call once connected (V019) |
