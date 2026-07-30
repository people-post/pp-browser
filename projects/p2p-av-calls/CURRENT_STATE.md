# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | **a3 done**; **a4 thin slice landed** — V020–V024 |
| ADRs | V001–V024 in [DECISIONS.md](DECISIONS.md) |
| a2/a3 media | LAN 1:1 voice + video OK (Android↔Win; Linux receive-only); NAT unclaimed |
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
| Signaling | `CallControlType::CallSfuAttach` + codec |
| UI | Still 1:1 chrome; multi-invite API exists (`StartCall` / `InviteParticipant`) |

## Still open (a4 polish / a5)

| Area | State |
|------|-------|
| Full multi-invite / group chrome | Pending |
| Dual `video_lo` + `video_hi` | a5 / V024 polish |
| App AEAD on SFU payloads under call media key | Follow-on |
| Roster proof auth (beyond `auth==call_id`) | Follow-on |
| Re-pick hop on failure cool-down | Partial (attach retries ranked hops) |
| ICE-fail 1:1 → SFU | Topology helper ready; not auto-wired from PC failed yet |

## Next agent — start here

1. Group / multi-invite UI + mid-call guest dogfood on seed SFU.  
2. Wire ICE-failed → `ShouldUseMediaRelay(..., ice_failed=true)`.  
3. Optional AEAD under call media key on SFU frames.  
4. a5: full lo/hi on both backends.

## Agent traps

| Wrong | Right |
|-------|-------|
| Adaptation only for group/SFU | Same V024 policy on **1:1 P2P** too |
| Force 1:1 via `media_relay` when ICE works | P2P backend; relay when N≥3 or ICE fail |
| Duplicate unrelated bitrate logic per path | One policy module, two backends |
| Relay assumes Opus/H264 | QoS `channel_type` only (N021) |
| Name relay API `keyframe` | Generic **`mark`** |
| Hardcoded N014 stages | Scorer + closed set (V023) |
