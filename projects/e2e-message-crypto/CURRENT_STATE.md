# Current state — as of 2026-07-06

Inventory of what exists in the codebase today for message encryption. Update when landing phase work.

**Planned:** full E2E stack in [DESIGN.md](DESIGN.md) — see [PHASES.md](PHASES.md).  
**Agent batch:** Waves **1** (**c1**) and **5** (**c2**) are **done**. Next: **c3** (PSK UX, verify gate, rotation) — see [chat-storage CURRENT_STATE § Next agent](../chat-storage-and-memory/CURRENT_STATE.md#next-agent--start-here).

## Release scope (v1 batch)

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| d0 (complete), c1, c2, c3 — **private `e2e` tier** | c3+ (`e2e_public` auto-key), c4 PQ |
| | Group E2E (E022 / D095 wire shape) |

## E2E crypto module

| Area | Status | Location |
|------|--------|----------|
| `base/crypto/` module | **Implemented** | `src/base/crypto/*` |
| libsodium vendored + linked | **Yes** | `third_party/libsodium/`, `src/base/CMakeLists.txt` |
| `IPskSessionStore` | **Interface + SQLite store** | `IPskSessionStore.h`, `SqlitePskSessionStore.*` |
| `MessageCipher` / AEAD | **Implemented** | `MessageCipher.*`, `EncryptedPayload.*` |
| Unit tests + frozen vectors | **7/7 pass** | `src/base/crypto/tests/crypto_vectors_test.cpp` |
| `docs/MESSAGE_ENCRYPTION.md` | **Promoted** | Stable spec |

## Messaging integration (c2)

| Area | Status | Location |
|------|--------|----------|
| Relay wire codec | **Implemented** | `E2eRelayPayloadCodec.*` — encrypt/decrypt `body.e2e.payload_b64` |
| Outbound encrypt (`channel == e2e`) | **Implemented** | `P2pMessagingService::SendUserMessage` |
| Inbound decrypt | **Implemented** | `RelayReceivePipeline::ProcessEnvelope` |
| History re-encrypt on export | **Implemented** | `ChatHistoryResponder`, `Libp2pChatHistoryService` |
| Directory signing key resolver | **Implemented** | `RelayDirectorySigningKeyResolver.*` |
| Hub wiring | **Implemented** | `MessagingHub` — `SqlitePskSessionStore`, resolver, `P2pMessagingService` |
| `e2e_public` on wire | **Plaintext** (unchanged until c3) | `RelayWirePayload` path |

## Related messaging (today)

| Area | Status | Location |
|------|--------|----------|
| Relay body wire shape | **`body.e2e.payload_b64`** (D090) | `ThreadTypes.h`, `ParseRelayEnvelope` |
| Payload bytes on wire (`e2e`) | **AEAD ciphertext** (base64 blob) | `E2eRelayPayloadCodec.*` |
| Outbound signing | **E014 canonical bytes** via `EnvelopeSigner` | `P2pMessagingService.cpp` |
| Inbound verify | **Implemented** (requires resolver / store entry) | `RelayReceivePipeline` step 2 |
| `Thread.encrypted` + `ThreadChannel` | **Set on direct threads** | chat-storage v2b |
| `sender_seq` / `session_epoch` on envelope | **On wire + AAD** | chat v6 |
| Tier split (`e2e` / `e2e_public`) | **Implemented** | [chat-storage CURRENT_STATE](../chat-storage-and-memory/CURRENT_STATE.md) |

## Identity and keys (today)

| Area | Status | Location |
|------|--------|----------|
| Local Ed25519 keypair | Implemented | `IdentityStore`, `Ed25519Signer` |
| Private key at rest | Base64 in JSON (not encrypted) | `identity.json` |
| Per-contact PSK persistence | **Store reads/writes `chat_targets`** | `SqlitePskSessionStore` — install UX in c3 |
| Peer public keys for verify | **Cache + lazy directory fetch** | `PeerSigningKeyStore`, `RelayDirectorySigningKeyResolver` |

## Third-party crypto libraries

| Library | Vendored | Linked to `pp_base` | Used for |
|---------|----------|---------------------|----------|
| BoringSSL | Yes (libp2p deps) | Yes | curl TLS, libp2p, `Ed25519Signer` |
| libsodium | Yes | **Yes** | E2E symmetric AEAD/HKDF (E002) |
| liboqs / PQ | No | — | Deferred (phase c4) |

## Tests

| Area | Location |
|------|----------|
| Ed25519 round-trip | `tests/messaging_foundation_test.cpp` |
| E2E crypto (frozen vectors) | `src/base/crypto/tests/crypto_vectors_test.cpp` — **7 tests** |
| Relay encrypt/decrypt + pipeline | `src/base/messaging/tests/e2e_relay_crypto_test.cpp` — **2 tests** |
| Chat sync (encrypted envelopes) | `src/base/messaging/tests/chat_sync_test.cpp` — **9 tests** |
| History responder (encrypted export) | `src/base/messaging/tests/chat_history_responder_test.cpp` |

## Known gaps (summary)

1. **c3** — PSK export/import, fingerprint gate, rotation UX; enable **`e2e_public`** send.
2. No end-to-end manual test with two profiles + live relay (planned c3).
3. PSK session store lacks dedicated unit tests (optional stretch).
4. Send fails with "PSK not configured" until user installs PSK (c3 UX).
5. **c1 rule still applies for new crypto code:** no `#include` of `ThreadTypes` / `P2pMessagingService` inside `base/crypto` (integration via feature/base-messaging layer).

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

## Phase c1 exit criteria

- [x] All c1 vector tests green (`pp_browser_crypto_vectors_test`)
- [x] Module usable without messaging includes in `base/crypto`
- [ ] PSK store round-trip tests (optional stretch — skeleton landed)

## Phase c2 exit criteria

- [x] `P2pMessagingService` encrypt/decrypt on `channel == e2e`
- [x] `RelayDirectorySigningKeyResolver` + inbound verify before decrypt
- [x] Fail closed on decrypt error
- [x] Automated round-trip + receive-pipeline tests green
- [ ] Manual two-profile relay exchange (deferred to c3 UX)
