# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a3 done**; **a4 gated on blind `media_relay`** — V020–V022) |
| ADRs | V001–V022 in [DECISIONS.md](DECISIONS.md) |
| UI | In-call **icon** mute/camera/leave + compact stacked bar; stage/PiP; **`<call-video-tile>` OnRender** + persistent GL tex (V018 letterbox) |
| Media stack | `CallMediaEngine` — Opus + **always H264 m-line** (V019); **1:1 PeerConnection** today |
| Platform HW | **Win MF + macOS/iOS VT + Linux VA-API + Android MediaCodec (NDK)** implemented |

## a2 / a3 closed

LAN 1:1 voice + video OK (Android↔Win bidirectional; Linux receive-only). NAT / SFU **not claimed**.

## a4 next (group ≤8 via blind forwarder)

| Area | State |
|------|-------|
| Topology | **V020/V021** — blind forwarder; 1:1 P2P; soft-migrate same `call_id` when N→≥3 |
| Mesh gate | **n4-media** / N018 — `media_relay` on `pp-node` + desktop (default on) |
| Privacy | Relay never holds call keys / never decodes payloads |
| Bandwidth / bills | **V022 / N019** — **A↑/A↓**, **B↑/B↓**, **C↑/C↓**; quote + ceiling; initiator pays; volunteer rate 0 |
| Pick / re-pick | Coordinator applies policy; **exact ranking / scorer still TBD** |
| Signaling | Invite / rotate / roster largely present from a1 |

## Next agent

1. Do **not** implement full-mesh or a media-aware SFU that needs call keys.  
2. Coordinate **n4-media** blind forwarder + ↑/↓ quote path with [p2p-mesh](../p2p-mesh/).  
3. **SFU pick-priority / pricing-scorer** — discuss next (do not invent final rank in code).  
4. No new video codec/device matrix in a4.

## Agent traps

| Wrong | Right |
|-------|-------|
| Full-mesh for group | Blind forwarder only (V020/V021) |
| Relay decodes or holds media keys | Opaque forward + call-key AEAD on clients |
| Single combined bps only | Separate **↑** and **↓** (V022) |
| Bill without quote / above ceiling | Quote + accept + hard ceiling (V022) |
| Classify audio vs video on relay | Byte-budget limits; Camera from **A↑** |
| End call when inviting a 3rd | Soft-migrate same `call_id` to SFU |
| Invent SFU pick order in code | Wait for priority design discussion |
