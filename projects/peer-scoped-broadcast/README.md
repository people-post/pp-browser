# Peer-scoped announce + live broadcast

**Status:** Design sketch accepted (2026-09-05) — **not implemented**  
**Owner:** Hongwei + agents  
**Related:** [p2p-mesh](../p2p-mesh/), [p2p-av-calls](../p2p-av-calls/), [content-cas](../content-cas/), [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md)

## One-line goal

Authenticated **per-PeerId announcement feeds** (optional voluntary helpers) plus **live video** on the existing **realtime** blind-hop path — not open GossipSub, not in-topic public replies.

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Planes, roles, whitelist, lifecycle, non-goals |

## Why this project exists

We considered classic gossip/pubsub (open topics, epidemic fan-out, in-topic replies) and rejected it as the product shape: amplification risk, topic collision/squatting, and weak lifecycle. The reduced model below keeps broadcast useful while matching mesh + calls + CAS boundaries already in L4.
