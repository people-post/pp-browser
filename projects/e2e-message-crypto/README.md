# E2E message encryption

**Status:** Planning — design in progress (as of 2026-06-29)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [chat-storage-and-memory](../chat-storage-and-memory/) (channel split, `sender_seq`, `session_epoch`, ingest rules)

## One-line goal

High-assurance **symmetric E2E** for direct chat: manual 256-bit PSK, HKDF-derived session keys, XChaCha20-Poly1305 AEAD with canonical AAD, replay binding via `sender_seq` — implemented in `base/crypto` (libsodium) and wired into messaging only after design and [chat-storage-and-memory](../chat-storage-and-memory/) channel/sync prerequisites are solid.

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
| d0 | Design baseline (this folder) | In progress |
| c1 | `base/crypto` groundwork (libsodium, no messaging wiring) | Not started |
| c2 | Messaging integration (encrypt body, decrypt on poll) | Not started |
| c3 | Key distribution UX (import, fingerprint, rotation) | Not started |
| c4 | Post-quantum migration (hybrid KEM / signatures) | Deferred |

Update this table when a phase completes.

## Open questions

- [ ] PSK at rest: file JSON vs OS keychain first?
- [ ] Relay field name for ciphertext (`body.ciphertext_b64` vs nested `body.e2e`)?
- [ ] Plaintext inside AEAD: raw UTF-8 `text` only v1, or JSON wrapper for `content_rml` from day one?
- [ ] Group E2E: defer indefinitely vs shared group PSK?
- [ ] When to add optional automated key setup (hybrid X25519 + ML-KEM) alongside manual PSK?
