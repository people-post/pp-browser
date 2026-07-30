# P2P A/V calls — current state

**Last updated:** 2026-07-30

## Landed

| Area | State |
|------|-------|
| Project docs | `projects/p2p-av-calls/` (v0 → **a3 done**; **a4 gated on blind `media_relay`** — V020–V023) |
| ADRs | V001–V023 in [DECISIONS.md](DECISIONS.md) |
| UI / media / HW | a3 LAN 1:1 path landed (see prior rows in git history / a3 section below) |

## a2 / a3 closed

LAN 1:1 voice + video OK (Android↔Win bidirectional; Linux receive-only). NAT / SFU **not claimed**.

## a4 next (group ≤8 via blind forwarder)

| Area | State |
|------|-------|
| Topology | **V020/V021** — blind forwarder; 1:1 P2P; soft-migrate when N→≥3 |
| Mesh gate | **n4-media** — `media_relay` on `pp-node` + desktop (default on) |
| Privacy | Relay never holds call keys / never decodes payloads |
| Bandwidth / bills | **V022 / N019** — ↑/↓ A/B/C; quote + ceiling; initiator pays |
| Hop pick | **V023 / N020** — short-term **contacts ∪ org seed** only; risk-aware score; pricing regulates later (not revenue-first) |
| Signaling | Invite / rotate / roster largely present from a1 |

## Next agent

1. Do **not** implement full-mesh, media-aware SFU, open public media_relay market, or `min(price)` sort.  
2. Ship n4-media + call consumer per V020–V023 / N018–N020.  
3. No new video codec/device matrix in a4.

## Agent traps

| Wrong | Right |
|-------|-------|
| Hardcoded N014 stage list for media | Scorer over closed eligibility (V023) |
| Open public / ultra-cheap wins | Short-term contacts ∪ seed only |
| Revenue-first paid SFU | Pricing **regulates** scarcity/abuse later |
| Full-mesh / decode on relay | Blind forwarder + call-key AEAD |
| Bill without quote | Quote + ceiling (V022) |
| Invent public directory in a4 | Mid-term N020 |
