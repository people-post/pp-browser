# ADP / AMP — Association Datagram Protocol + mesh stack

**Status:** L1 foundation landed; **AMP stack spec locked** (D0)  
**Owner:** Hongwei + agents  
**Stable refs:** [ADP.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/ADP.md) (L1) · [AMP-SESSION.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-SESSION.md) (L2) · [AMP-CHANNEL.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/contracts/AMP-CHANNEL.md) (L3)  
**Related:** [STACK.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/STACK.md), [libp2p-pq-transport](../_archive/libp2p-pq-transport/), [p2p-av-calls](../p2p-av-calls/), [p2p-mesh](../p2p-mesh/)

## One-line goal

**ADP** — Asio-free UDP L1 (HMAC, path migrate, BE+reliable).  
**AMP** — Full peer mesh on ADP: Session (L2 full) + Channel (L3) + existing app protocols (L4). Replaces TCP + Noise + Yamux.

## Documents in this folder

The stack itself — its big picture, contracts, design, the transport's decisions, test matrices and open items — is
pp-cpp-amp's and lives there ([docs index](https://github.com/people-post/pp-cpp-amp/blob/main/README.md#docs), [CR026](../capability-repos/DECISIONS.md#cr026--pp-cpp-amp-takes-the-stacks-design-and-the-contracts-it-alone-implements)); a one-line stub here at each old
path points to it. This folder keeps pp-browser's adoption of the stack.

| File | Purpose |
|------|---------|
| [CURRENT_STATE.md](CURRENT_STATE.md) | What landed, step by step, as the app moved onto Amp |
| [PHASES.md](PHASES.md) | L1 phases + AMP migration D0–D10 (done) |
| [DECISIONS.md](DECISIONS.md) | The app's decisions on its use of the stack (A006, A008–A011, A017, A020–A023, A028); the transport's (A001–A029, the rest) keep a heading pointing to pp-cpp-amp |
| [L4_PROTOCOL_KINDS.md](../../docs/contracts/L4_PROTOCOL_KINDS.md) | L4 conversation kinds + new-`protocol_id` gate (A028) |
| [CALL_MEDIA_CIRCUIT.md](CALL_MEDIA_CIRCUIT.md) | Amp call-media over circuit — nested Session (A024) |
| [EXTRACTION_READINESS.md](EXTRACTION_READINESS.md) | Finished: the record of moving the stack out to `pp-cpp-amp` |

In pp-cpp-amp: [STACK](https://github.com/people-post/pp-cpp-amp/blob/main/docs/STACK.md) (big picture), [DESIGN](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DESIGN.md) (L1), [DECISIONS](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md), [test matrices](https://github.com/people-post/pp-cpp-amp/blob/main/docs/testing/TEST_MATRIX.md), [STATUS](https://github.com/people-post/pp-cpp-amp/blob/main/docs/STATUS.md).

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| 0–3 | ADP L1 foundation | **Done** |
| 4 | Opus TCP side-path (transitional) | Code landed; superseded by AMP D6 |
| D0 | Stack constitution | **Done** |
| D1–D6 | Session, channel, link, chat, call-media | **Done** (parallel stacks; product still libp2p) |
| D7a | Circuit tunnel AMP (A022 coordinator) | **Done** |
| D7b | Media-relay AMP coordinator | **Done** |
| D8 | ch0 capability exchange + addr ingest | **Done** (dial-back + UPnP UDP chrome landed) |
| D9 | MeshHost/CallStack cutover | **Done** through step 7 — Amp owns mesh when stack starts; Identify/TCP listen skipped; dogfood deleted |
| D10 | Clean TCP mesh drop | **Done** — Amp hard-require; no product Host; LAN mDNS/PreferLocal Amp-only; TCP compose tests deleted |
| A017 | libp2p shrink (PeerId only) | **Done** — Host/TCP/Yamux/Noise purged; PeerId + wire retained |

## Locked product decisions

- **ADP** = L1 only (`src/lib/amp/L1/`, `pp_base_adp`)
- **AMP** = L1–L4 stack; see [STACK.md](https://github.com/people-post/pp-cpp-amp/blob/main/docs/STACK.md)
- L2 **full** Session (MSH + AEAD) — [A013](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a013--l2-full-session-only)
- One assoc per peer; many channels — [A014](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a014--one-association-per-peer-pair)
- Fragmentation at L3 — [A018](https://github.com/people-post/pp-cpp-amp/blob/main/docs/DECISIONS.md#a018--fragmentation-at-l3-not-l1)
- libp2p Host/stream path retires — [A017](DECISIONS.md#a017--libp2p-shrink-retain-crypto--peerid-only)
