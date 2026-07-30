# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a3 done**; **a4 gated on blind `media_relay`** — V020/V021) |
| ADRs | V001–V021 in [DECISIONS.md](DECISIONS.md) |
| UI | In-call **icon** mute/camera/leave + compact stacked bar; stage/PiP; **`<call-video-tile>` OnRender** + persistent GL tex (V018 letterbox) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); **1:1 PeerConnection** today |
| Platform HW | **Win MF + macOS/iOS VT + Linux VA-API + Android MediaCodec (NDK)** implemented |

## a2 / a3 closed

LAN 1:1 voice + video OK (Android↔Win bidirectional; Linux receive-only). NAT / SFU **not claimed**.

## a4 next (group ≤8 via blind forwarder)

| Area | State |
|------|-------|
| Topology | **V020/V021** — blind selective forwarder; **no** full-mesh; 1:1 stays P2P; soft-migrate same `call_id` when N→≥3 |
| Mesh gate | **n4-media** / N018 — `media_relay` on `pp-node` + desktop (default on) |
| Privacy | Relay never holds call keys / never decodes payloads |
| Bandwidth | Client disables Camera when hop budget too small; relay limits by **bytes** |
| Pick / re-pick | Coordinator policy (exact ranking **TBD**) |
| Signaling | Invite / rotate / roster largely present from a1 |

## Next agent

1. Do **not** implement full-mesh group media or a media-aware SFU that needs call keys.  
2. Coordinate **n4-media** blind forwarder with [p2p-mesh](../p2p-mesh/).  
3. SFU pick-priority design still open — discuss before coding rank.  
4. No new video codec/device matrix in a4.

## Agent traps

| Wrong | Right |
|-------|-------|
| Full-mesh for group | Blind forwarder only (V020/V021) |
| Relay decodes or holds media keys | Opaque forward + call-key AEAD on clients |
| Classify audio vs video on relay | Byte-budget limits; client Camera policy |
| End call when inviting a 3rd | Soft-migrate same `call_id` to SFU |
| Separate audio/video relay pipelines | One `media_relay` module (N018) |
| Invent SFU pick order in code | Wait for priority design discussion |
