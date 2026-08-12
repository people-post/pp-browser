# Multi-device account

**Status:** Design freeze (m0) — identity/key model agreed; implementation not started  
**Owner:** Hongwei + agents  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D096→D099), [e2e-message-crypto](../e2e-message-crypto/) (E025), [at-rest-crypto](../at-rest-crypto/) (A010), [push-notifications](../push-notifications/) (device wake)

## One-line goal

One portable **Account ID** across linked devices, with **per-device Peer IDs**, shared **DEK** (per-device vault wrap), **account-signed** envelopes, and **private PSKs not auto-synced** — so multi-device feels like one person without libp2p id conflicts.

## Release scope (this project)

| In | Out (for now) |
|----|----------------|
| Account ID format + account vs device key split | Nickname / directory cosmetics |
| Brief: one Account ID → one `relay:` binding per server | Full multi-relay product UX |
| Shared DEK; per-device `vault.bin` | PIN recovery / cloud vault backup |
| Private (`e2e`) PSK not auto-synced | Optional private PSK sync |
| Public(/group) PSK sync policy (direction) | Complete inbox fan-out implementation |
| Link-device ritual (design) | Calls multi-ring polish |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Account/device boundary, ids, keys, Brief binding |
| [DECISIONS.md](DECISIONS.md) | ADRs (M001+) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Code today vs target |
| [PHASES.md](PHASES.md) | Delivery checklist |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| m0 | Identity/key design freeze | **Done** (docs) |
| m1 | Account ID + key split in types/storage | Not started |
| m2 | Wire / `ChatTargetKey` hard cut to Account ID | Not started |
| m3 | Brief register binding + directory endpoints | Not started |
| m4 | Link-device + DEK seal + sync policy | Not started |

## Normative promotion

When phases ship, promote Account ID format, register binding, and wire identity into [`docs/contracts/`](../../docs/contracts/) (WIRE_SCHEMAS, SERVICE_ENDPOINTS, AT_REST_ENCRYPTION, DATA_LAYOUT) and amend [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md). Keep rationale here; do not duplicate normative tables.
