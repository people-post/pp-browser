# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | **a3 done**; **a4 gated** — **V020–V024** |
| ADRs | V001–V024 in [DECISIONS.md](DECISIONS.md) |
| a2/a3 media | LAN 1:1 voice + video OK (Android↔Win; Linux receive-only); NAT unclaimed |

## a4 next

| Area | State |
|------|-------|
| Topology | 1:1 P2P; N≥3 / ICE-fail → `media_relay`; soft-migrate (V020/V021) |
| Hop pick | Contacts ∪ org seed (V023 / N020) |
| Budgets | ↑/↓ quote when hop used (V022 / N019) |
| Framing | N021 on **SFU path only** |
| Adaptive A/V | **V024 — one policy, two backends** (1:1 P2P + SFU); audio ≫ lo ≫ hi; producer first |
| Codecs | Reuse a3 Opus + H264 |

## Next agent — start here

1. Read **V024** end-to-end: shared adaptation module; do **not** build group-only rate control.  
2. Mesh **n4-media** N018–N021 for hop; call consumer wires SFU backend to same policy as P2P.  
3. Soft-migrate 1:1→group must keep publish/demand roles (V021 + V024).  
4. a4 may ship single video layer before full lo/hi on both backends.  
5. **Do not:** full-mesh; decode on relay; open public market; `min(price)`; new codecs; force all 1:1 through relay.

## Agent traps

| Wrong | Right |
|-------|-------|
| Adaptation only for group/SFU | Same V024 policy on **1:1 P2P** too |
| Force 1:1 via `media_relay` when ICE works | P2P backend; relay when N≥3 or ICE fail |
| Duplicate unrelated bitrate logic per path | One policy module, two backends |
| Relay assumes Opus/H264 | QoS `channel_type` only (N021) |
| Name relay API `keyframe` | Generic **`mark`** |
| Relay-only adaptation | Producer rate control first |
| Hardcoded N014 stages | Scorer + closed set (V023) |
