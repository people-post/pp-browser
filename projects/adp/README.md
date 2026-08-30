# ADP — Association Datagram Protocol

**Status:** Foundation landed (Phases 0–3)  
**Owner:** Hongwei + agents  
**Stable refs:** [ADP.md](../../docs/contracts/ADP.md)  
**Related:** [libp2p-pq-transport](../libp2p-pq-transport/) (L2 mesh crypto), [p2p-av-calls](../p2p-av-calls/) (lossy media), [pp-ledger platform-integration](https://github.com/people-post/pp-ledger/blob/develop/docs/platform-integration.md) (future shared stack)

## One-line goal

Asio-free, message-oriented UDP **connection** layer with HMAC sender-binding, path migration, best-effort + reliable delivery — L1 only; product crypto stays L2.

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Wire, API, I/O, delivery contracts |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Rollout checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs A001–A007 |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| 0 | Project + ADRs | **Done** |
| 1 | Codec + HMAC + DatagramIo + best-effort | **Done** |
| 2 | Reliable ACK/rtx | **Done** |
| 3 | Harden + contract doc + OsUdp smoke | **Done** |

## Locked product decisions

- Name / folders: **ADP** (`src/base/adp/`, `pp_base_adp`)
- L1 HMAC binding ≠ content encryption
- Message-oriented connection; movable path
- Asio-free `DatagramIo`; tests use `MemoryDatagramIo`
