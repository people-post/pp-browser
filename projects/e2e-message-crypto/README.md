# E2E message encryption

**Status:** Wave 6 (**c3**) complete (2026-07-06) — private `e2e` PSK UX landed  
**Owner:** Hongwei + agents  
**Stable refs:** [MESSAGE_ENCRYPTION.md](../../docs/contracts/MESSAGE_ENCRYPTION.md), [WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md), [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md), [DATA_LAYOUT.md](../../docs/contracts/DATA_LAYOUT.md)  
**Related projects:** [chat-storage-and-memory](../chat-storage-and-memory/) (three tiers D089, `ChatPayload`, `sender_seq`, ingest rules, identity-keyed `ChatTargetKey` D079); [multi-device-account](../multi-device-account/) (account signing E025; account KEM M015; private PSK not auto-synced)

## One-line goal

High-assurance **symmetric E2E** for all P2P tiers: manual PSK (private), automated keys (public), pairwise sender-keys (group); HKDF session keys, XChaCha20-Poly1305 AEAD, `body.e2e.payload_b64` wire format, binary `ChatPayload` plaintext (E010/D087). See [E021](DECISIONS.md#e021--three-chat-tiers-both-direct-tiers-e2e-d089).

## Release scope (v1 batch)

**c1–c3** (private `e2e` tier) with chat v2b + v6. Exclude unless expanded: c3+ public auto-key, c4 PQ, group (E022). Coordinated via [chat-storage agent waves](../chat-storage-and-memory/PHASES.md#agent-batch-delivery-order).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Target crypto architecture, wire format, threat model, post-quantum posture |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Design-first roadmap, progress checklists, **[agent batch waves](PHASES.md#agent-batch-delivery-order)** |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| d0 | Design baseline (this folder) | **Complete** |
| c1 | `base/crypto` groundwork (libsodium, no messaging wiring) | **Done** (PSK store tests optional) |
| c2 | Messaging integration (AEAD on wire, verify) | **Done** (2026-07-06) |
| c3 | Key distribution UX — **private tier** (import, fingerprint, rotation) | **Done** (2026-07-06) |
| c3+ | **Public tier** auto-key (`e2e_public`) | **Next** (post-v1 unless expanded) |
| c4 | Post-quantum migration (hybrid KEM / signatures) | Deferred |

## Design decisions

_All planning questions (O001–O007) resolved — see [DECISIONS.md](DECISIONS.md)._

**Resolved:** three tiers E021/D089/D090; E023 no `public_relay`; ciphertext field → `body.e2e.payload_b64` (E009); AEAD plaintext → binary `ChatPayload` (E010/D087); private PSK UX → generate, export, import, verify (E011); public auto-key → hybrid KEM + signing resolver (E013/E024, O007); public device-lock rekey → E027; CAIP-10 blockchain contact ids (D091); group pairwise → E022; rich OOB bundle on rotation (E020/D086); Ed25519 sign bytes → fixed binary layout + BLAKE2b body hash (E014); HKDF `info` → `channel` + `epoch` only (E015); peer signing keys → `IPeerSigningKeyResolver` + `PeerSigningKeyStore` (E016/E024); relay-user identity value → `relay:<opaque_id>` (E017 / D082); retired PSK ledger on `rotate_psk` (E018 / D083); PSK store in `profile.db` `chat_targets` (E008 / D084); passive epoch advance (E019 / D085).

## d0 exit

- [x] AEAD / blob codec frozen test vectors in [DESIGN.md](DESIGN.md) § Test vectors (regenerate with [`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py))
- [x] Stable spec promoted to [docs/contracts/MESSAGE_ENCRYPTION.md](../../docs/contracts/MESSAGE_ENCRYPTION.md)
