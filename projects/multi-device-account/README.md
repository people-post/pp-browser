# Multi-device account

**Status:** Design freeze (m0) + **M009–M019** / **D100**; **m1–m2b** + **m4a–m4b** done; next **m3** `endpoints[]` (**M017**) then **m4c** paste contacts (**M018**)  
**Owner:** Hongwei + agents  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D096→D100), [e2e-message-crypto](../e2e-message-crypto/) (E025), [at-rest-crypto](../at-rest-crypto/) (A010), [push-notifications](../push-notifications/) (device wake)

## One-line goal

One portable **Account ID** across linked devices, with **per-device Peer IDs**, shared **DEK** (per-device vault wrap), **account KEM** for public/group auto-key, **account-signed** envelopes, and **private PSKs not auto-synced** — so multi-device feels like one person without libp2p id conflicts.

## Release scope (this project)

| In | Out (for now) |
|----|----------------|
| Account ID format + account vs device key split | Full multi-relay product UX |
| Brief: Account-first directory early (M011); one Account ID → one `relay:` binding; **m3 `endpoints[]` (M017)** | Nickname cosmetics beyond search/`q=` |
| Wire/catalog hard cut to Account ID (M009/M010) | **`e2e_public` send** enablement |
| Shared DEK; per-device `vault.bin`; link-device paste (M012) | PIN recovery / cloud vault backup |
| Private (`e2e`) PSK not auto-synced | Optional private PSK sync; D074 dual-writer |
| Contacts + public thread index in paste (**M018**) | Transcripts in paste / cloud CRDT |
| Unlink spec (**M019**); phase 1 after m3 | KEM rotation revoke; remote wipe; Account ID rotation |
| Dogfood: one active sender (**M016**) | Call multi-ring polish |

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
| m0 | Identity/key + contact/directory ADR freeze | **Done** (docs; M016–M019 added) |
| m1 | Account ID + key split in types/storage | **Done** (PQ signing hard cut) |
| m2a | Brief by-account + Account-first search | **Done** |
| m2b | Wire / `ChatTargetKey` hard cut to Account ID | **Done** |
| m4a | Soft inbox ack (shared mailbox; M013) | **Done** |
| m4b | Link-device + DEK seal + sync policy | **Done** (first-run paste; QR later) |
| m3 | Directory `endpoints[]` (M017) + one-sender help (M016) | **Next** |
| m4c | Paste contacts + public thread index (M018) | After m3 |
| m4d | Unlink phase 1 (M019) | After m3 |

## Normative promotion

When phases ship, promote Account ID format, register binding, `endpoints[]`, and wire identity into [`docs/contracts/`](../../docs/contracts/) (WIRE_SCHEMAS, SERVICE_ENDPOINTS, AT_REST_ENCRYPTION, DATA_LAYOUT) and amend [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md). Keep rationale here; do not duplicate normative tables.
