# Current state — as of 2026-07-02

Inventory of what exists in the codebase today for message encryption. Update when landing phase work.

**Planned:** full E2E stack in [DESIGN.md](DESIGN.md) — see [PHASES.md](PHASES.md).  
**Implementation:** ready — wave 1 (`c1`) per [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order), parallel with chat `v2a-core`.

## Release scope (v1 batch)

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| d0 (complete), c1, c2, c3 — **private `e2e` tier** | c3+ (`e2e_public` auto-key), c4 PQ |
| | Group E2E (E022 / O008) |

## E2E crypto module

| Area | Status | Location |
|------|--------|----------|
| `base/crypto/` module | **Not implemented** | — |
| libsodium vendored | **Yes** — not linked to `pp_base` yet | `third_party/libsodium/`, `cmake/dependencies.cmake`; c1 task = link + module |
| `IPskSessionStore` | **Not implemented** | — |
| `MessageCipher` / AEAD | **Not implemented** | — |
| Unit tests + frozen vectors | **Not implemented** | Spec + vectors in [DESIGN.md](DESIGN.md), [MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md) |
| `docs/MESSAGE_ENCRYPTION.md` | **Promoted** | Stable spec — [docs/MESSAGE_ENCRYPTION.md](../../docs/MESSAGE_ENCRYPTION.md) |

## Related messaging (today)

| Area | Status | Location |
|------|--------|----------|
| Relay body | Plaintext `text` | `RelayMessageBody` in `src/base/messaging/ThreadTypes.h` |
| Outbound signing | Ed25519 sign envelope JSON | `P2pMessagingService.cpp`, `Ed25519Signer.cpp` |
| Inbound verify | **Not implemented** | `Ed25519Signer::Verify` only in tests |
| `Thread.encrypted` | Schema only; never `true` | `ThreadTypes.h` |
| `channel` / e2e thread split | **Not implemented** | [chat-storage CURRENT_STATE](../chat-storage-and-memory/CURRENT_STATE.md) |
| `sender_seq` / `session_epoch` | **Not implemented** | Design in chat-storage v6 |
| Target wire body | **`body.e2e.payload_b64`** only (D090) | [WIRE_SCHEMAS](../chat-storage-and-memory/WIRE_SCHEMAS.md) — baseline code not cut over |

## Identity and keys (today)

| Area | Status | Location |
|------|--------|----------|
| Local Ed25519 keypair | Implemented | `IdentityStore`, `Ed25519Signer` |
| Private key at rest | Base64 in JSON (not encrypted) | `identity.json` — field `encrypted_private_key_b64` |
| Per-contact PSK | **Not implemented** | — |
| Peer public keys for verify | **Not implemented** (spec: [E016](DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add)) | `PeerSigningKeyStore` planned; contacts/directory hits lack `signing_public_key_b64` today |

## Third-party crypto libraries

| Library | Vendored | Linked to `pp_base` | Used for |
|---------|----------|---------------------|----------|
| BoringSSL | Yes (libp2p deps) | Yes | curl TLS, libp2p, `Ed25519Signer` |
| libsodium | Yes (`third_party/libsodium`) | **No** (c1) | E2E symmetric AEAD/HKDF (E002) |
| liboqs / PQ | No | — | Deferred (phase c4) |

## Tests

| Area | Location |
|------|----------|
| Ed25519 round-trip | `tests/messaging_foundation_test.cpp` |
| E2E crypto (frozen vectors) | Planned: `src/base/crypto/tests/` (c1) |

## Known gaps (summary)

1. No symmetric E2E — relay sees all direct message text.
2. No `base/crypto` module — application AEAD layer not wired.
3. No PSK storage or fingerprint UX.
4. Ed25519 signing without inbound verify on relay poll (no peer key store yet — E016).
5. Messaging schema lacks `channel`, `sender_seq`, `session_epoch` (chat-storage tracks).
6. PQ: only classical Ed25519 for envelopes; no hybrid plan in code (documented in DESIGN).
7. **Identity model:** baseline code keys threads on local `Contact.id`; target uses identity-keyed `ChatTargetKey` (chat-storage D079) with wire `sender_contact_id` = communicating identity value (`relay:<opaque_id>` per D082/E017).
8. **c1 must not include** `ThreadTypes` / `P2pMessagingService` — keep `base/crypto` isolated until c2.

## Design completion checklist (phase d0)

- [x] Project folder + README, DESIGN, DECISIONS, PHASES, CURRENT_STATE
- [x] Threat model and PQ posture documented
- [x] HKDF labels and AAD byte layout specified (E015: `channel` + `epoch` only)
- [x] Encrypted payload blob layout specified
- [x] Resolve O001/O002 — E009/E010 in [DECISIONS.md](DECISIONS.md)
- [x] Resolve O003/O005 — E011/E012 in [DECISIONS.md](DECISIONS.md)
- [x] Resolve O004 — E013 in [DECISIONS.md](DECISIONS.md)
- [x] Resolve O007 — E024 (auto-key trust anchor) + D091 (CAIP-10) in [DECISIONS.md](DECISIONS.md)
- [x] Resolve peer signing key binding — E016 in [DECISIONS.md](DECISIONS.md)
- [x] Ed25519 signing frozen test vectors (hex) in DESIGN.md § Test vectors
- [x] HKDF frozen test vector in DESIGN.md § Test vectors (E015)
- [x] Retired PSK ledger on `rotate_psk` — E018 in [DECISIONS.md](DECISIONS.md)
- [x] PSK store in `profile.db` `chat_targets` — E008/D084
- [x] Rich OOB PSK bundle (multi-hop rotation) — E020/D086
- [x] Passive epoch advance / envelope epoch decrypt — E019/D085
- [x] AEAD / blob codec frozen test vectors in DESIGN.md § Test vectors ([`tools/gen_aead_vectors.py`](tools/gen_aead_vectors.py))
- [x] Promote stable spec to `docs/MESSAGE_ENCRYPTION.md`
