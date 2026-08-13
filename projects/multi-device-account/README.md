# Multi-device account

**Status:** Design freeze (m0) + **M009–M012** / **D100**; **m1** PQ signing done; next **m2a** Brief directory then **m2b** wire  
**Owner:** Hongwei + agents  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D096→D100), [e2e-message-crypto](../e2e-message-crypto/) (E025), [at-rest-crypto](../at-rest-crypto/) (A010), [push-notifications](../push-notifications/) (device wake)

## One-line goal

One portable **Account ID** across linked devices, with **per-device Peer IDs**, shared **DEK** (per-device vault wrap), **account-signed** envelopes, and **private PSKs not auto-synced** — so multi-device feels like one person without libp2p id conflicts.

## Release scope (this project)

| In | Out (for now) |
|----|----------------|
| Account ID format + account vs device key split | Full multi-relay product UX |
| Brief: Account-first directory early (M011); one Account ID → one `relay:` binding | Nickname cosmetics beyond search/`q=` |
| Wire/catalog hard cut to Account ID (M009/M010) | **`e2e_public` send** enablement |
| Shared DEK; per-device `vault.bin` (design; m4) | PIN recovery / cloud vault backup |
| Private (`e2e`) PSK not auto-synced | Optional private PSK sync |
| Link-device ritual design (**M012**; implement m4) | Calls multi-ring polish |

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
| m0 | Identity/key + contact/directory ADR freeze | **Done** (docs) |
| m1 | Account ID + key split in types/storage | **Done** (PQ signing hard cut) |
| m2a | Brief by-account + Account-first search | **Next** |
| m2b | Wire / `ChatTargetKey` hard cut to Account ID | Not started |
| m3 | Multi-device directory attach polish | Not started |
| m4 | Link-device + DEK seal + sync policy | Not started (M012 frozen) |

## Normative promotion

When phases ship, promote Account ID format, register binding, and wire identity into [`docs/contracts/`](../../docs/contracts/) (WIRE_SCHEMAS, SERVICE_ENDPOINTS, AT_REST_ENCRYPTION, DATA_LAYOUT) and amend [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md). Keep rationale here; do not duplicate normative tables.
