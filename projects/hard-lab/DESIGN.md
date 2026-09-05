# Hard lab — design

**Authoritative scenario ladder / topology / profiles:** [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md).  
**This file** records project intent, ownership, and non-goals for delivery.  
**Execution order:** [PHASES.md](PHASES.md). **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **ADRs:** [DECISIONS.md](DECISIONS.md).

---

## Problem

IMAGE_SMOKE L0–L2 and host-side `B-CALL-HOP` prove a **packaged hop** is usable from the probe host. They do **not** prove two **mutually unreachable** peers can find each other and complete call/chat under bad paths — the deployment risk we care about.

Loopback partition fixtures already own SoftMigrate/circuit **policy**. Hard lab owns **namespace + packaging + path quality** truth.

---

## Goals

1. Forced-hop topology (no A↔B route) with thin probes.
2. Orthogonal profiles: topology × link impairment × discovery mode.
3. Purpose-IDed scenarios (`N-HARD-*` / `B-HARD-*`) with sparse CI release set.
4. Extend later to multi-hop circuit, directory/DHT discovery, and NAT-shaped labs without a second harness family.

## Non-goals

- Replacing in-process gtests or ADP loss matrices with Docker.
- Full GUI as the lab client.
- PR-blocking netem / multi-hop before Wave 1 is boringly green.
- Claiming carrier CGNAT / hole-punch coverage before those features ship.
- Multi-SFU media bitpaths (see [MULTI_HOP_CIRCUIT.md](../media-hop-reachability/MULTI_HOP_CIRCUIT.md)).

---

## Ownership

| Layer | Owns |
|-------|------|
| **This project** | Harness delivery, scenario scripts, compose family, purpose inventory updates |
| **media-hop-reachability** | Dial-by-PeerId, circuit evolution, SoftMigrate consume contracts |
| **p2p-mesh** | Relay scope, admission policy, DHT/directory product rules |
| **p2p-av-calls** | Call lifecycle product criteria consumed by `B-HARD-*` |
| **ops docs** | Cadence / pass-fail tables in [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) |

---

## Wave summary

```text
Wave 0  Prerequisites (loopback, L0–L2, B-CALL-HOP)     — keep green
Wave 1  Forced hop clean          N/B-HARD force/call/msg
Wave 2  Lossy / asym / bw
Wave 3  Stale / seed / dir / DHT / admit-hard
Wave 4  Multi-hop circuit         (after L3.5)
Wave 5  NAT shapes / UPnP / v6    (weekly/manual)
Wave 6  Product stress on hard topo
Wave 7  Horizons                  (placeholders)
```

Full tables: [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md).
