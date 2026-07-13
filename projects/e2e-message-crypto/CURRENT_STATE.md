# Current state — as of 2026-07-06

Inventory of what exists in the codebase today for message encryption. Update when landing phase work.

**Planned:** full E2E stack in [DESIGN.md](DESIGN.md) — see [PHASES.md](PHASES.md).  
**Agent batch:** Waves **1** (**c1**), **5** (**c2**), and **6** (**c3**) are **done**. Next: **c3+** (`e2e_public` auto-key) or post-v1 unless expanded.

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
| `PskBundleCodec` (E020) | **Implemented** | `PskBundleCodec.*` |
| Unit tests + frozen vectors | **7/7 pass** | `src/base/crypto/tests/crypto_vectors_test.cpp` |
| `docs/contracts/MESSAGE_ENCRYPTION.md` | **Promoted** | Stable crypto contract |
| `docs/contracts/WIRE_SCHEMAS.md` | **Promoted** | Normative wire (was under chat-storage) |
| `docs/contracts/COMPATIBILITY.md` | **Policy** | Dirty disk + newer peer/API |

## Messaging integration (c2)

| Area | Status | Location |
|------|--------|----------|
| Relay wire codec | **Implemented** | `E2eRelayPayloadCodec.*` |
| Outbound encrypt (`channel == e2e`) | **Implemented** | `P2pMessagingService::SendUserMessage` |
| Inbound decrypt | **Implemented** | `RelayReceivePipeline::ProcessEnvelope` |
| History re-encrypt on export | **Implemented** | `ChatHistoryResponder`, `Libp2pChatHistoryService` |
| Directory signing key resolver | **Implemented** | `RelayDirectorySigningKeyResolver.*` |
| Hub wiring | **Implemented** | `MessagingHub` |

## PSK UX (c3)

| Area | Status | Location |
|------|--------|----------|
| Generate on secure-thread open | **Implemented** | `PskSessionCoordinator::EnsureGenerated`, `ContactActionDispatcher`, `ContactsController` |
| Export raw base64 + fingerprint | **Implemented** | `chat.rml` banner, `ChatController::OnCopyPskKey` |
| Import raw base64 or bundle JSON | **Implemented** | `PskSessionCoordinator`, `ChatController::OnImportPsk` |
| Verify gate (`psk_verified_at`) | **Implemented** | `ChatController::OnVerifyPsk`, send blocked until verified |
| `rotate_psk` + bundle export | **Implemented** | `PskSessionCoordinator::RotatePskAndExportBundle`, compromised banner |
| Signing key fingerprint on add-contact | **Implemented** (display-only) | `contact_detail.rml`, `ContactsController` |
| `e2e_public` send | **Still disabled** | c3+ auto-key |

## Related messaging (today)

| Area | Status | Location |
|------|--------|----------|
| Payload bytes on wire (`e2e`) | **AEAD ciphertext** | `E2eRelayPayloadCodec.*` |
| Outbound signing | **E014 canonical bytes** | `EnvelopeSigner`, `P2pMessagingService` |
| Inbound verify | **Cache + lazy directory** | `PeerSigningKeyStore`, `RelayDirectorySigningKeyResolver` |
| Tier split (`e2e` / `e2e_public`) | **Implemented** | chat-storage v2b |

## Tests

| Area | Location |
|------|----------|
| E2E crypto (frozen vectors) | `src/base/crypto/tests/crypto_vectors_test.cpp` — **7 tests** |
| PSK bundle codec | `src/base/crypto/tests/psk_bundle_codec_test.cpp` — **3 tests** |
| PSK session coordinator | `src/feature/messaging/tests/psk_session_coordinator_test.cpp` — **1 test** |
| Relay encrypt/decrypt + pipeline | `src/feature/messaging/tests/e2e_relay_crypto_test.cpp` — **2 tests** |
| Chat sync (encrypted envelopes) | `src/feature/messaging/tests/chat_sync_test.cpp` — **13 tests** |
| Cross-cutting ingest | `src/feature/messaging/tests/messaging_cross_cutting_test.cpp` — **6 tests** |
| Live relay (D093, env-gated) | `src/base/messaging/tests/relay_live_integration_test.cpp` |

## Known gaps (summary)

1. **c3+** — `e2e_public` hybrid KEM auto-key and send enablement.
2. QR encode/decode for PSK (optional stretch).
3. Manual two-profile live-relay walkthrough (dev QA).
4. **c1 rule still applies:** no messaging includes inside `base/crypto`.

## Phase c3 exit criteria

- [x] Generate/export/import PSK UX
- [x] Fingerprint verify gate before send
- [x] `rotate_psk` bundle export/import with retired tail cap
- [x] Compromised-banner rotate path
- [x] Automated coordinator tests green
- [ ] QR OOB (deferred)
