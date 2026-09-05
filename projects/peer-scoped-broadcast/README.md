# Peer-scoped announce + live broadcast

**Status:** Design sketch accepted (2026-09-05); **Spine B exit met; Spine C in progress** (tip→arm→accept without SoftMigrate) — see [CURRENT_STATE](CURRENT_STATE.md)
**Owner:** Hongwei + agents  
**Related:** [p2p-mesh](../p2p-mesh/), [p2p-av-calls](../p2p-av-calls/), [content-cas](../content-cas/), [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md)

## One-line goal

Authenticated **per-PeerId announcement feeds** (optional voluntary helpers) plus **live video** on the existing **realtime** blind-hop path — not open GossipSub, not free-speak replies; public on-screen chat only via publisher-signed tips.

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Planes, roles, pickup UX, replies/overlay, whitelist, lifecycle, non-goals |
| [MEDIA_TREE.md](MEDIA_TREE.md) | Multi-hop blind SFU tree for massive live audiences (Spine F) |
| [DECISIONS.md](DECISIONS.md) | ADRs B001–B006 (broadcast ≠ call; keep AEAD; ticket key; leaf attach) |
| [PHASES.md](PHASES.md) | Media-scale checklist B0–B3 |
| [PROGRAM.md](PROGRAM.md) | Multi-project spines — overall progress over single-project speed |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Implementation progress |

Product notes locked in design:

- **Pickup:** Notifications tab + optional live banner — not call ringing ([DESIGN § Product pickup UX](DESIGN.md#product-pickup-ux--not-call-ringing)).
- **Replies:** Private DM, or on-screen via publisher-mediated rebroadcast + rate limits ([DESIGN § Speak / reply](DESIGN.md#speak--reply-rules)).
- **Heartbeats:** publisher-paced re-announce while live ([DESIGN § Heartbeat](DESIGN.md#live-re-announce-heartbeat)).
- **Scale:** degree-capped media tree after Spine C ([MEDIA_TREE.md](MEDIA_TREE.md); program Spine F).

## Why this project exists

We considered classic gossip/pubsub (open topics, epidemic fan-out, in-topic replies) and rejected it as the product shape: amplification risk, topic collision/squatting, and weak lifecycle. The reduced model below keeps broadcast useful while matching mesh + calls + CAS boundaries already in L4.
