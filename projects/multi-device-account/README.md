# Multi-device account

**Status:** **m1–m3**, **m4a–m4b**, per-device sessions **d1–d2**, the master device **d4** (as a policy) and account state **M023** (but its easy tier) done; **next:** the M023 easy tier (needs a device), d6, and the cryptographic d4. Read **M021** (sessions per device pair; supersedes M003 / M015 §2–3 / M016 / M020 / D074 as its phases ship) and **M023** (recovery phrase + encrypted account locker; supersedes M018 and the local account backup) before anything older. Status detail: [CURRENT_STATE.md](CURRENT_STATE.md)  
**Owner:** Hongwei + agents  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (D096→D100), [e2e-message-crypto](../e2e-message-crypto/) (E025), [at-rest-crypto](../at-rest-crypto/) (A010), [push-notifications](../push-notifications/) (device wake)

## One-line goal

One portable **Account ID** across linked devices, with **per-device Peer IDs** and keys, sessions **per device pair** (M021), envelopes signed by the sending device and checked against the account-signed device list, shared **DEK** (per-device vault wrap), and account state recoverable from a **recovery phrase** (M023) — so multi-device feels like one person without libp2p id conflicts.

## Release scope (this project)

| In | Out (for now) |
|----|----------------|
| Account ID format + account vs device key split | Full multi-relay product UX |
| Brief: Account-first directory early (M011); one Account ID → one `relay:` binding; **m3 `endpoints[]` (M017)** | Nickname cosmetics beyond search/`q=` |
| Wire/catalog hard cut to Account ID (M009/M010) | **`e2e_public` send** enablement |
| Shared DEK; per-device `vault.bin`; link-device paste (M012) | PIN-wrapped key backup (rejected — M023 uses a recovery phrase) |
| Private (`e2e`) PSK not auto-synced | Optional private PSK sync; D074 dual-writer |
| Account state via the encrypted locker + change log (**M023**; M018 paste superseded) | Message history sync |
| Unlink spec (**M019**); phase 1 after m3 | KEM rotation revoke; remote wipe; Account ID rotation |
| Every device sends (**M021** d2; M016's one-sender limit superseded) | Call multi-ring polish (d5) |

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
| m3 | Directory `endpoints[]` (M017) + one-sender help (M016) | **Done** (hard cut; no top-level `peer_id`) |
| m4c | Paste contacts + public thread index (M018) | **Superseded** by M023 — not built |
| m4d | Unlink phase 1 (M019) | Folded into d4 (revoke) |
| d1–d2 | Device keys + list; sessions per device pair (**M021**, **M022**) | **Done** |
| d4 | Master device, revoke, restore signs out the others (**M021**) | **Done as a policy** — the account key still on every device (see PHASES) |
| d3, d5 | Siblings (sent copies, the missing-copy ask), calls ringing every device (**M021**) | **Done** |
| d6 | Directory carries the card; card version in envelopes; E027 re-decided | Later — see PHASES |
| s1–s6 | Recovery phrase, account locker, contact sync, restore, saved copies (**M023**) | **Built** but the easy tier (synced keychain) and device checks of the pickers — see PHASES |

## Normative promotion

When phases ship, promote Account ID format, register binding, `endpoints[]`, and wire identity into [`docs/contracts/`](../../docs/contracts/) (WIRE_SCHEMAS, SERVICE_ENDPOINTS, AT_REST_ENCRYPTION, DATA_LAYOUT) and amend [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md). Keep rationale here; do not duplicate normative tables.
