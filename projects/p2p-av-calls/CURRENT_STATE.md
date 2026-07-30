# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a3 done**; a4 next) |
| ADRs | V001–V019 in [DECISIONS.md](DECISIONS.md) |
| UI | In-call **icon** mute/camera/leave + compact stacked bar; stage/PiP; **`<call-video-tile>` OnRender** + persistent GL tex (V018 letterbox) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera; `CameraCaptureOrientation` (Android `SENSOR_ORIENTATION` + display rotation; iOS interface orientation); `IVideoCodec` factory |
| Platform HW | **Win MF + macOS/iOS VT + Linux VA-API + Android MediaCodec (NDK)** implemented |
| iOS wiring | `NSMicrophoneUsageDescription` + `NSCameraUsageDescription`; `AVAudioSession` play-and-record; `UIBackgroundModes` `audio` |
| CMake | `SDL_CAMERA ON`; MF/`VideoToolbox`/`mediandk`+`camera2ndk` linked; Android `RECORD_AUDIO` + `CAMERA` |

## a2 closed (LAN voice)

| Area | State |
|------|-------|
| Two-device LAN Opus | **OK (2026-07-28)** — Win↔Linux; host ICE |
| NAT / mobile voice | **Not claimed** |

## a3 closed (LAN 1:1 video)

| Area | State |
|------|-------|
| Unified Opus+H264 SDP | **Done** in `CallMediaEngine` |
| SDL camera + local preview frames | **Done** |
| Camera / mute content + roster | **Done** |
| Shell Camera + stage chrome | **Done** — compact icon chrome dogfooded |
| Capture orientation + letterbox tiles | **Done** — `CameraCaptureOrientation_*`; `CallVideoTileRenderer` contain-fit |
| Mobile UI orientation | **Locked portrait** (Android manifest + iOS plist + `SDL_HINT_ORIENTATIONS`) — free rotation deferred (EGL/call crash class) |
| GL persistent texture tiles (V018) | **Done** — `<call-video-tile>` `OnRender` + app-owned GL tex (`CallVideoTileRenderer`) |
| Win MF / macOS VT / Android MediaCodec / Linux VA-API | **Code landed** |
| iOS plist + AVAudioSession | **Done** — device dogfood optional (wiring-only exit) |
| LAN video dogfood | **OK (2026-07-30)** — see below; NAT / seed SFU **not claimed** |

## a3 LAN video dogfood

| Path | State |
|------|-------|
| Android local preview (camera on) | **OK (2026-07-30)** |
| Android ↔ Windows | **OK (2026-07-30)** — bidirectional video both ways |
| Android → Linux / Windows → Linux | **OK (2026-07-30)** — one-way only (Linux dogfood host has **no camera** → no send; receive/display OK) |
| Linux → Android / Linux → Windows | **Not claimed** — no capture on dogfood Linux host |
| macOS / iOS device | **Not claimed** (iOS wiring complete; device dogfood optional) |
| NAT / seed SFU | **Not claimed** |

**Accepted Linux limits (V017 / V019):** Video send needs a camera **and** usable HW H264 encoder. Dogfood Linux lacked a camera (one-way receive only). Hosts without VA-API/usable encoder may also fail send; voice continues either way. Do **not** claim NAT or seed SFU.

## Next agent — a4 scoping + mesh SFU

1. Agree a4 delivery slice (LAN full-mesh group vs wait on mesh **n4** SFU) — see discussion / upcoming V020.
2. Group ≤8: multi-invite, guests, rotate-on-leave (signaling largely present); multi-peer media + roster UI still open.
3. Do **not** claim NAT / mobile group until org-seed `audio_relay` / `video_relay` (mesh n4 + V008).

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera; hardcode mobile sensor angles (use `CameraCaptureOrientation`).

## Agent traps

| Wrong | Right |
|-------|-------|
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Remount shell for every video frame | DirtyWindow + persistent texture (V018) |
| Stretch video to fill tile | Letterbox/pillarbox in `CallVideoTileRenderer` |
| Hardcode Android front=270 / back=90 | `ACAMERA_SENSOR_ORIENTATION` + display rotation via `CameraCaptureOrientation` |
| Rely on free device rotation during calls | Keep portrait lock until EGL + live capture re-orient are hardened |
| Hide Camera on voice-started calls | Same in-call once connected (V019) |
| Defer iOS plist/session from a3 | iOS wiring is a3 exit (V016, 2026-07-30) |
| Single-row text Mute/Camera/Leave on compact | Icon buttons + stacked call bar |
| Claim Linux→peer video without camera/encoder | Document one-way; voice continues (V017/V019) |
