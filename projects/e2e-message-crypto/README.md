# E2E message encryption

**Status:** Planning — design in progress (as of 2026-06-29)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [chat-storage-and-memory](../chat-storage-and-memory/) (channel split, `ChatPayload`, `sender_seq`, ingest rules)

## One-line goal

High-assurance **symmetric E2E** for direct chat: manual 256-bit PSK, HKDF session keys, XChaCha20-Poly1305 AEAD, `body.e2e.payload_b64` wire format, JSON `ChatPayload` plaintext (E010).

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

## Open questions

| ID | Topic |
|----|-------|
| **E-O003** | PSK entry UX v1 (paste base64 vs fingerprint confirm vs QR) |
| **E-O004** | Automated key agreement timing (manual only vs hybrid KEM in c4) |
| **E-O005** | Group E2E strategy (deferred vs shared PSK vs MLS) |

**Resolved:** ciphertext field → `body.e2e.payload_b64` (E009); AEAD plaintext → JSON `ChatPayload` (E010).
