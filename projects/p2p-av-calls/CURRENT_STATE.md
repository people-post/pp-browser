# P2P A/V calls — current state

**Last updated:** 2026-07-31

Dogfood / codebase board for **this week**. Stable code map: [docs/architecture/CALLS.md](../../docs/architecture/CALLS.md). Product rules: [DESIGN.md](DESIGN.md) / [DECISIONS.md](DECISIONS.md).

## Landed

| Area | State |
|------|-------|
| Project docs | **a3 done**; **a4 thin slice landed** — V020–V024; code map in [CALLS.md](../../docs/architecture/CALLS.md) |
| ADRs | V001–V024 in [DECISIONS.md](DECISIONS.md) |
| a2/a3 media | LAN 1:1 voice + video — **same matrix as Windows for macOS**: Android↔Win / Android↔Mac bidirectional; ↔Linux voice OK, video receive-only when Linux has no camera; Linux↔Mac / Linux↔Win OK 2026-07-31; NAT still unclaimed |
| **a4 thin** | Soft-migrate to `media_relay` when N≥3; shared V024 adaptation; SFU engine mode (see below) |

## a4 thin in code

| Area | State |
|------|-------|
| Topology | 1:1 P2P when N=2; N≥3 → coordinator `RankMediaHops` → quote/attach → `call_sfu_attach` fan-out (V021) |
| Hop pick | Contacts ∪ org seed via mesh `MeshHopPolicy` (V023 / N020) |
| Budgets | Quote A↑/A↓ applied into adaptation; volunteer rate 0 |
| Framing | N021 on SFU path — audio ch0 `reliable_ordered`, video ch1 `latest_lossy` |
| Adaptive A/V | `CallMediaAdaptation` (audio ≫ video_lo); `CallMediaEngine::ApplyAdaptation` / `StartSfu` |
| Codecs | Reuse a3 Opus + H264 |
| Signaling | `CallControlType::CallSfuAttach` + codec; early `call_sdp`/`call_ice` buffered until PC `Start` |
| UI | Group call chrome / roster / mid-call invite API; full multi-party polish still open |

## Recent hardening (2026-07-31)

| Issue | Mitigation |
|-------|------------|
| Android↔Mac LAN ICE | macOS `NSLocalNetworkUsageDescription` in packaged Info.plist |
| Linux dial → Mac stuck Connecting | Preserve buffered remote SDP/ICE across PC rebuild; offerer SDP re-send; duplicate offer ignored |
| 1:1 ICE fail / hang on Connecting | 15s timeout + honest “Couldn't connect”; Retry rebuilds P2P as offerer; platform tip (Local Network / mic / firewall); do not auto-leave |
| 1:1 hit “group needs media_relay” | SFU attach-wait / `sfu_hint` only for N≥3; do not treat PC `closed` as 1:1→SFU |
| Soft-migrate fail on 3rd joiner | Eject joiner; keep existing 1:1 P2P; invite preflight when no hop |

## Still open (a4 polish / a5)

| Area | State |
|------|-------|
| Full multi-invite / group chrome dogfood on seed SFU | Pending |
| Dual `video_lo` + `video_hi` | a5 / V024 polish |
| App AEAD on SFU payloads under call media key | Follow-on |
| Roster proof auth (beyond `auth==call_id`) | Follow-on |
| Re-pick hop on failure cool-down | Partial (attach retries ranked hops) |
| ICE-fail **1:1** → SFU when hop exists | Helper exists; auto path intentionally **not** used for N=2 (avoid false group toasts); N≥3 ICE-fail → SFU wired |
| `CallSessionManager` extract | Target in [CALLS.md](../../docs/architecture/CALLS.md) — TopologyController + P2pSignalingBridge |

## Next agent — start here

1. Group / multi-invite UI + mid-call guest dogfood on seed SFU.  
2. Optional: extract `CallTopologyController` (behavior-preserving) per CALLS.md.  
3. Optional AEAD under call media key on SFU frames.  
4. a5: full lo/hi on both backends.

## Agent traps

| Wrong | Right |
|-------|-------|
| Adaptation only for group/SFU | Same V024 policy on **1:1 P2P** too |
| Force 1:1 via `media_relay` when ICE works | P2P backend; SFU when N≥3 (or explicit future NAT path) |
| 1:1 ICE fail / `closed` → SFU attach-wait | N≥3 only; else leave or stay P2P |
| Duplicate unrelated bitrate logic per path | One policy module, two backends |
| Relay assumes Opus/H264 | QoS `channel_type` only (N021) |
| Name relay API `keyframe` | Generic **`mark`** |
| Hardcoded N014 stages | Scorer + closed set (V023) |
| Edit product rules only in CALLS.md | Product → DESIGN/DECISIONS; code map → CALLS; dogfood → this file |
