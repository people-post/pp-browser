# E2E message encryption

**Status:** Design baseline complete (d0) — ready for c1 implementation  
**Owner:** Hongwei + agents  
**Stable refs:** [MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md), [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [chat-storage-and-memory](../chat-storage-and-memory/) (channel split, `ChatPayload`, `sender_seq`, ingest rules, identity-keyed `ChatTargetKey` D079)

## One-line goal

High-assurance **symmetric E2E** for direct chat: manual 256-bit PSK, HKDF session keys, XChaCha20-Poly1305 AEAD, `body.e2e.payload_b64` wire format, binary `ChatPayload` plaintext (E010/D087).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Target crypto architecture, wire format, threat model, post-quantum posture |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today (no E2E crypto yet) |
| [PHASES.md](PHASES.md) | Design-first roadmap and progress checklists |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| d0 | Design baseline (this folder) | **Complete** |
| c1 | `base/crypto` groundwork (libsodium, no messaging wiring) | Not started |
| c2 | Messaging integration (encrypt body, decrypt on poll) | Not started |
| c3 | Key distribution UX (import, fingerprint, rotation) | Not started |
| c4 | Post-quantum migration (hybrid KEM / signatures) | Deferred |

## Design decisions

_All planning questions (O001–O006) resolved — see [DECISIONS.md](DECISIONS.md)._

**Resolved:** ciphertext field → `body.e2e.payload_b64` (E009); AEAD plaintext → binary `ChatPayload` (E010/D087); PSK establishment UX → generate, export, import, verify (E011); rich OOB bundle on rotation (E020/D086); group E2E → out of scope (E012); automated key agreement → optional hybrid KEM in c4 (E013); Ed25519 sign bytes → fixed binary layout + BLAKE2b body hash (E014); HKDF `info` → `channel` + `epoch` only (E015); peer signing keys → relay directory + `PeerSigningKeyStore` + OOB fingerprint at add (E016); relay-user identity value → `relay:<opaque_id>` (E017 / D082); retired PSK ledger on `rotate_psk` (E018 / D083); PSK store in `profile.db` `chat_targets` (E008 / D084); passive epoch advance (E019 / D085).

## d0 exit

- [x] AEAD / blob codec frozen test vectors in [DESIGN.md](DESIGN.md) § Test vectors (regenerate with [`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py))
- [x] Stable spec promoted to [docs/MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md)
