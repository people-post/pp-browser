# ADP / AMP — Association Datagram Protocol + mesh stack

**Status:** L1 foundation landed; **AMP stack spec locked** (D0)  
**Owner:** Hongwei + agents  
**Stable refs:** [ADP.md](../../docs/contracts/ADP.md) (L1) · [AMP-SESSION.md](../../docs/contracts/AMP-SESSION.md) (L2) · [AMP-CHANNEL.md](../../docs/contracts/AMP-CHANNEL.md) (L3)  
**Related:** [STACK.md](STACK.md), [libp2p-pq-transport](../libp2p-pq-transport/), [p2p-av-calls](../p2p-av-calls/), [p2p-mesh](../p2p-mesh/)

## One-line goal

**ADP** — Asio-free UDP L1 (HMAC, path migrate, BE+reliable).  
**AMP** — Full peer mesh on ADP: Session (L2 full) + Channel (L3) + existing app protocols (L4). Replaces TCP + Noise + Yamux.

## Documents in this folder

| File | Purpose |
|------|---------|
| [STACK.md](STACK.md) | **Big picture** — four layers, objects, QoS, transition |
| [DESIGN.md](DESIGN.md) | L1 wire, API, I/O, delivery |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | L1 phases + AMP migration D0–D9 |
| [DECISIONS.md](DECISIONS.md) | ADRs A001–A020 |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| 0–3 | ADP L1 foundation | **Done** |
| 4 | Opus TCP side-path (transitional) | Code landed; superseded by AMP D6 |
| D0 | Stack constitution | **Done** |
| D1–D6 | Session, channel, link, chat, call-media | **Done** (parallel stacks; product still libp2p) |
| D7a | Circuit tunnel AMP (A022 coordinator) | **Done** |
| D7b | Media-relay AMP coordinator | **Done** |
| D8 | ch0 capability exchange + addr ingest | **Partial** — dial-back deferred (not blocking D9); LAN mDNS advertises `amp_udp` |
| D9 | MeshHost/CallStack cutover | **In progress** — chat/history on Amp; call-media next |

## Locked product decisions

- **ADP** = L1 only (`src/base/adp/`, `pp_base_adp`)
- **AMP** = L1–L4 stack; see [STACK.md](STACK.md)
- L2 **full** Session (MSH + AEAD) — [A013](DECISIONS.md#a013--l2-full-session-only)
- One assoc per peer; many channels — [A014](DECISIONS.md#a014--one-association-per-peer-pair)
- Fragmentation at L3 — [A018](DECISIONS.md#a018--fragmentation-at-l3-not-l1)
- libp2p Host/stream path retires — [A017](DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)
