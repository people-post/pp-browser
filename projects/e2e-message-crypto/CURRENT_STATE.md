# Current state — as of 2026-07-06

Inventory of what exists in the codebase today for message encryption. Update when landing phase work.

**Planned:** full E2E stack in [DESIGN.md](DESIGN.md) — see [PHASES.md](PHASES.md).  
**Agent batch:** Waves **1** (**c1**), **5** (**c2**), and **6** (**c3**) are **done**. Public auto-key (c3+) and group pairwise E2E are in tree. Next: **c3++** device-lock rekey (E027) polish / c4 PQ.

## Release scope

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| d0 (complete), c1, c2, c3 — **private `e2e` tier** | c4 PQ |
| **`e2e_public` auto-key (E024)** — compose + ingest for direct threads | |
| **Group pairwise E2E (E022/D095)** — N ciphertexts per member, membership events | MLS / sender-key tree |

## E2E crypto module

| Area | Status | Location |
|------|--------|----------|
| `foundation/crypto/` module | **Implemented** | `src/foundation/crypto/*` |
| libsodium vendored + linked | **Yes** | `third_party/libsodium/`, `src/base/CMakeLists.txt` |
| `IPskSessionStore` | **Interface + SQLite store** | `IPskSessionStore.h`, `SqlitePskSessionStore.*` |
| `MessageCipher` / AEAD | **Implemented** | `MessageCipher.*`, `EncryptedPayload.*` |
| `PskBundleCodec` (E020) | **Implemented** | `PskBundleCodec.*` |
| Unit tests + frozen vectors | **7/7 pass** | `src/foundation/crypto/tests/crypto_vectors_test.cpp` |
| `docs/contracts/MESSAGE_ENCRYPTION.md` | **Promoted** | Stable crypto contract |
| `docs/contracts/WIRE_SCHEMAS.md` | **Promoted** | Normative wire (was under chat-storage) |
| `docs/contracts/COMPATIBILITY.md` | **Policy** | Dirty disk + newer peer/API |

## Messaging integration (c2)

| Area | Status | Location |
|------|--------|----------|
| Relay wire codec | **Implemented** | `E2eRelayPayloadCodec.*` |
| Outbound encrypt (`channel == e2e`) | **Implemented** | `MeshDeliveryOrchestrator::SendUserMessage` |
| Outbound encrypt (`channel == e2e_public`) | **Implemented** | Auto-key encapsulation in `SendUserMessage` |
| Group outbound encrypt (pairwise per member) | **Implemented** | `MeshDeliveryOrchestrator::SendGroupMessage`, `GroupE2ePayloadCodec` |
| Inbound decrypt | **Implemented** | `RelayReceivePipeline::ProcessEnvelope` |
| History re-encrypt on export | **Implemented** | `ChatHistoryResponder`, `Libp2pChatHistoryService` |
| Directory signing key resolver | **Implemented** | `RelayDirectorySigningKeyResolver.*` |
| Hub wiring | **Implemented** | `ConversationsHub` |

## PSK UX (c3)

| Area | Status | Location |
|------|--------|----------|
| Generate on secure-thread open | **Implemented** | `PskSessionCoordinator::EnsureGenerated`, `ContactActionDispatcher`, `ContactsController` |
| Export raw base64 + fingerprint | **Implemented** | `chat.rml` banner, `ChatController::OnCopyPskKey` |
| Import raw base64 or bundle JSON | **Implemented** | `PskSessionCoordinator`, `ChatController::OnImportPsk` |
| Verify gate (`psk_verified_at`) | **Implemented** | `ChatController::OnVerifyPsk`, send blocked until verified |
| `rotate_psk` + bundle export | **Implemented** | `PskSessionCoordinator::RotatePskAndExportBundle`, compromised banner |
| Signing key fingerprint on add-contact | **Implemented** (display-only) | `contact_detail.rml`, `ContactsController` |
| `e2e_public` send | **Implemented** (auto-key) | `MeshDeliveryOrchestrator::SendUserMessage` |
| Public device-lock rekey (E027) | **Next** | `PublicPskLockCoordinator` |

## Related messaging (today)

| Area | Status | Location |
|------|--------|----------|
| Payload bytes on wire (`e2e`) | **AEAD ciphertext** | `E2eRelayPayloadCodec.*` |
| Outbound signing | **E014 canonical bytes** | `EnvelopeSigner`, `MeshDeliveryOrchestrator` |
| Inbound verify | **Cache + lazy directory** | `PeerSigningKeyStore`, `RelayDirectorySigningKeyResolver` |
| Tier split (`e2e` / `e2e_public`) | **Implemented** | chat-storage v2b |

## Tests

| Area | Location |
|------|----------|
| E2E crypto (frozen vectors) | `src/foundation/crypto/tests/crypto_vectors_test.cpp` — **7 tests** |
| PSK bundle codec | `src/foundation/crypto/tests/psk_bundle_codec_test.cpp` — **3 tests** |
| PSK session coordinator | `src/feature/conversations/tests/psk_session_coordinator_test.cpp` — **1 test** |
| Relay encrypt/decrypt + pipeline | `src/feature/conversations/tests/e2e_relay_crypto_test.cpp` — **2 tests** |
| Chat sync (encrypted envelopes) | `src/feature/conversations/tests/chat_sync_test.cpp` — **13 tests** |
| Cross-cutting ingest | `src/feature/conversations/tests/messaging_cross_cutting_test.cpp` — **6 tests** |
| Live relay (D093, env-gated) | `src/base/messaging/tests/relay_live_integration_test.cpp` |

## Known gaps (summary)

1. **c3++** — public 1:1 device-lock rekey (E027).
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
