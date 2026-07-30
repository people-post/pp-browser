# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a3 done**; **a4 gated on true SFU** — V020) |
| ADRs | V001–V020 in [DECISIONS.md](DECISIONS.md) |
| UI | In-call **icon** mute/camera/leave + compact stacked bar; stage/PiP; **`<call-video-tile>` OnRender** + persistent GL tex (V018 letterbox) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); SDL camera; `CameraCaptureOrientation`; `IVideoCodec` factory — **1:1 PeerConnection** today |
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
| Unified Opus+H264 SDP | **Done** |
| Shell Camera + stage / orientation / letterbox | **Done** |
| LAN video dogfood | **OK (2026-07-30)** — Android↔Win bidirectional; Linux receive-only (no camera); NAT / SFU **not claimed** |

## a4 next (group ≤8 via true SFU)

| Area | State |
|------|-------|
| Topology decision | **Locked V020** — true SFU; **no** full-mesh; audio+video; reuse a3 codecs |
| Mesh gate | Needs **n4-media** (N017): volunteer SFU on `pp-node` + desktop Node checkboxes |
| Signaling (invite / rotate / roster) | Largely present from a1 |
| Call SFU consumer + multi-tile UI | **Not started** |
| SFU choice priority | **TBD** — design discussion (N014 placeholder only) |

## Next agent

1. Do **not** implement full-mesh group media.  
2. Mesh **n4-media** (true SFU) + call consumer are the a4 path — coordinate with [p2p-mesh](../p2p-mesh/).  
3. SFU pick-priority design is a separate discussion (do not invent ranking in code yet).  
4. No new video codec/device matrix in a4.

**Do not:** OpenH264 as product default; fail call when H264 HW missing; renegotiate SDP for camera; hardcode mobile sensor angles; ship N≥3 full-mesh.

## Agent traps

| Wrong | Right |
|-------|-------|
| Full-mesh PeerConnections for group | True SFU only (V020) |
| TURN-as-SFU (N−1 uploads) | Selective forward — one uplink per participant |
| Block a4 on peer message_relay or paid pricing | Media SFU volunteer first (N017) |
| Expand encode matrix for new devices in a4 | Reuse a3 Opus + H264 HW (V017–V019) |
| Audio-only SDP for voice + renegotiate on Camera | Always Opus+H264 initial SDP (V019) |
| Fail `Start()` when H264 HW missing | Advertise video; encode/decode best-effort (V019) |
| Claim Linux→peer video without camera/encoder | Document one-way; voice continues |
