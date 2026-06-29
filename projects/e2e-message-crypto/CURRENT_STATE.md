# Current state — as of 2026-06-29

Inventory of what exists in the codebase today for message encryption. Update when landing phase work.

**Planned:** full E2E stack in [DESIGN.md](DESIGN.md) — see [PHASES.md](PHASES.md).

## E2E crypto module

| Area | Status | Location |
|------|--------|----------|
| `base/crypto/` module | **Not implemented** | — |
| libsodium vendored | **Not implemented** | — |
| `IPskSessionStore` | **Not implemented** | — |
| `MessageCipher` / AEAD | **Not implemented** | — |
| Unit tests + frozen vectors | **Not implemented** | — |
| `docs/MESSAGE_ENCRYPTION.md` | **Not implemented** | Spec lives in [DESIGN.md](DESIGN.md) until promoted |

## Related messaging (today)

| Area | Status | Location |
|------|--------|----------|
| Relay body | Plaintext `text` | `RelayMessageBody` in `src/base/messaging/ThreadTypes.h` |
| Outbound signing | Ed25519 sign envelope JSON | `P2pMessagingService.cpp`, `Ed25519Signer.cpp` |
| Inbound verify | **Not implemented** | `Ed25519Signer::Verify` only in tests |
| `Thread.encrypted` | Schema only; never `true` | `ThreadTypes.h` |
| `channel` / e2e thread split | **Not implemented** | [chat-storage CURRENT_STATE](../chat-storage-and-memory/CURRENT_STATE.md) |
| `sender_seq` / `session_epoch` | **Not implemented** | Design in chat-storage |

## Identity and keys (today)

| Area | Status | Location |
|------|--------|----------|
| Local Ed25519 keypair | Implemented | `IdentityStore`, `Ed25519Signer` |
| Private key at rest | Base64 in JSON (not encrypted) | `identity.json` — field `encrypted_private_key_b64` |
| Per-contact PSK | **Not implemented** | — |
| Peer public keys for verify | **Not implemented** | Contacts have relay IDs, not signing keys |

## Third-party crypto libraries

| Library | Vendored | Used for |
|---------|----------|----------|
| BoringSSL | Yes (libp2p deps) | curl TLS, libp2p, `Ed25519Signer` |
| libsodium | **No** | Planned for E2E symmetric (E002) |
| liboqs / PQ | **No** | Deferred (phase c4) |

## Tests

| Area | Location |
|------|----------|
| Ed25519 round-trip | `tests/messaging_foundation_test.cpp` |
| E2E crypto | None |

## Known gaps (summary)

1. No symmetric E2E — relay sees all direct message text.
2. No libsodium — no application AEAD layer.
3. No PSK storage or fingerprint UX.
4. Ed25519 signing without inbound verify on relay poll.
5. Messaging schema lacks `channel`, `sender_seq`, `session_epoch` (chat-storage tracks).
6. PQ: only classical Ed25519 for envelopes; no hybrid plan in code (documented in DESIGN).

## Design completion checklist (phase d0)

- [x] Project folder + README, DESIGN, DECISIONS, PHASES, CURRENT_STATE
- [x] Threat model and PQ posture documented
- [x] HKDF labels and AAD byte layout specified
- [x] Encrypted payload blob layout specified
- [x] Resolve O001/O002 — E009/E010 in [DECISIONS.md](DECISIONS.md)
- [ ] Resolve O003–O005 (PSK UX, automated KEM, group E2E)
- [ ] Frozen test vectors (hex) appended to DESIGN.md
- [ ] Promote stable spec to `docs/MESSAGE_ENCRYPTION.md` when d0 exits
