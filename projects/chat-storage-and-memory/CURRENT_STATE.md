# Current state — as of 2026-07-06

Inventory of what exists in the codebase today. Update this file when landing phase work.

**Planned but not implemented:** see [DESIGN.md](DESIGN.md) and D008–D068 in [DECISIONS.md](DECISIONS.md).  
**Agent batch:** Waves **1–2** merged; **Wave 3** landed; **Wave 4** (v6-schema through **v6-integrity**) landed; **Wave 5** (e2e **c2**) landed — see [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order).

## Next agent — start here

| Priority | Work | Blocked by |
|----------|------|------------|
| **Wave 6** | [e2e c3](../e2e-message-crypto/PHASES.md#phase-c3--key-distribution-ux) — PSK UX, verify gate, rotation | c2 (done) |
| **UX gaps (v2a-core)** | Composer `maxlength` (`kMaxComposeTextBytes`) | — |

**Key paths (wave 5 — c2):**

- Wire codec: `src/base/messaging/E2eRelayPayloadCodec.*`
- Signing resolver: `src/feature/messaging/RelayDirectorySigningKeyResolver.*`
- Send/receive: `P2pMessagingService.cpp`, `RelayReceivePipeline.cpp`
- PSK store: `src/feature/messaging/SqlitePskSessionStore.*`
- Tests: `e2e_relay_crypto_test`, `chat_sync_test`, `chat_history_responder_test`

**Interim behaviors (do not “fix” without reading DECISIONS):**

- **`e2e`** relay body is **AEAD ciphertext** in `payload_b64` (c2 landed).
- **`e2e_public`** threads exist and show tier badge; **compose/send disabled** until c3 auto-key; wire stays plaintext.
- Inbound poll: **find-only** via `chat_targets` (no auto-create thread on unknown sender — D062).
- libp2p peer-direct history (D060): **`Libp2pChatHistoryService`** on `/pp-browser/chat-history/1.0.0`; register dial endpoints via `P2pMessagingService::RegisterPeerDirectEndpoint`.
- Relay history: **`HttpRelayClient::FetchChatHistory`** shipped; external relay D027 ready for integration tests (D093).

## Release scope (v1 batch)

**Bucket B** ([D092](DECISIONS.md#d092--release-scope-bucket-b)):

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| chat v2a–v6 + post-v4, post-v6b/c/d | `e2e_public` auto-key (c3+), group E2E |
| e2e c1–c3 (private `e2e` tier) | c4 PQ |
| AI storage + memory (v3) | |
| libp2p peer-direct history (D060) — **required** (D094) | |

## Persistence

| Area | Status | Location |
|------|--------|----------|
| **SqliteThreadStore** (v2a) | **Implemented** | `src/base/messaging/SqliteThreadStore.*` |
| Legacy `JsonThreadStore` | Retained for tests | `src/base/messaging/JsonThreadStore.*` |
| Profile-scoped paths | Implemented | `{data_dir}/profiles/{id}/threads/` — [CONFIGURATION.md](../../docs/CONFIGURATION.md) |
| `IThreadStore` interface | Extended (routing, outbox) | `src/base/messaging/IThreadStore.h` |
| SQLite + libsodium on `pp_base` | **Linked** | `src/base/CMakeLists.txt` |
| `MessagingLimits.h` | **Implemented** | poll/gap/outbox caps (D029/D041) |
| Durable `ConversationSummary` on disk | **Implemented** (v3) | `GetThreadMemory` / `SetThreadMemory` |
| Clear history + **history floor** (D037) | **Implemented** | `ClearMessages` sets `history_floor_seq = loaded_max_seq` |
| Windowed transcript load | **Partial** | `GetMessagesPage`; UI default page size |
| Agent context + compaction | **Implemented** (v3) | `ThreadCompactionService`, summary injection |

## Data model (today)

### `Thread` — `src/base/messaging/ThreadTypes.h`

- **`ThreadChannel`**: `None`, `E2e`, `E2ePublic`
- **`peer_identity_kind` / `peer_identity_value`** on direct threads
- **`DirectChatTarget`** — lookup key for `chat_targets`

### `ThreadMessage`

- **`sender_seq` / `session_epoch`** on E2E relay-visible rows (v6-schema)
- **`transport`** column (`local` / `relay` / `direct`)
- **`GetMessagesBySeqRange`** for tail/gap queries

### `RelayEnvelope` (v1 wire — D063/D090)

- E014 signing via **`EnvelopeSigner`** on outbound (no JSON `dump()`)
- **`ChatHistoryRequest` / `ChatHistoryResponse`** + `FetchChatHistory` on `IRelayClient`

## Messaging and routing

| Feature | Status | Location |
|---------|--------|----------|
| Relay send + poll (v1 envelope) | **Implemented** | `P2pMessagingService.*` |
| **E014 outbound signing** | **Implemented** | `EnvelopeSigner`, `P2pMessagingService` |
| **Inbound receive pipeline** | **Implemented** | `RelayReceivePipeline`, `E2eIngestClassifier`, `ReplayWindow` |
| **Inbound Ed25519 verify** | **Implemented** (fail closed if key missing) | `PeerSigningKeyStore`, `RelayDirectorySigningKeyResolver`, `RelayReceivePipeline` |
| **AEAD payload in `payload_b64` (`e2e`)** | **Implemented** (c2) | `E2eRelayPayloadCodec.*`, `P2pMessagingService`, `RelayReceivePipeline` |
| **`e2e_public` plaintext on wire** | **Implemented** (until c3) | `RelayWirePayload.*` |
| Local write + outbox | **Implemented** | `AppendMessage`, `ReconcileOutbox` |
| Inbound find-only routing (D062) | **Implemented** | `RelayReceivePipeline` |
| **History floor on clear** (D037) | **Implemented** | `SqliteThreadStore::ClearMessages` |
| **Poll backoff 2 s** + batch cap (D032/D029) | **Implemented** | `P2pMessagingService` |
| **`FetchChatTargetMessages`** / tail sync | **Implemented** | `ChatSyncService` — relay fetch + gap repair + user sync |
| **User-initiated sync UX** (D059) | **Implemented** | `chat.rml` — Sync with peer / Retry sync banner |
| **Tail sync on open + reconnect** | **Implemented** | `OnSelectThread`, `SetRelayClient`, messaging init |
| libp2p history (D060) | **Implemented** | `Libp2pChatHistoryService`, `ChatHistoryResponder` — peer-first in `ChatSyncService` |
| Relay history fetch (D027) | **Implemented** (`HttpRelayClient`) | D093 — live relay ready |
| Integrity banners (D068) | **Implemented** | `chat.rml` compromised banner; `EpochBumpCoordinator`; passive adopt in `RelayReceivePipeline` |
| Gap detection on live ingest | **Partial** | classifier + auto `RepairGap` on `AcceptGap` |

## UI (today)

| Feature | Status | Location |
|---------|--------|----------|
| Sidebar tier badges | **Implemented** | `sidebar.rml`, `ChatController` |
| **`e2e_public` compose disabled** | **Implemented** | `compose_disabled` |
| Clear history / forget memory | **Implemented** | `chat.rml`, `InboxController` |
| Composer maxlength | **Not wired** | `kMaxComposeTextBytes` |
| Delivery / transport badges | Not implemented | post-v6d |

## Tests

| Area | Location | Notes |
|------|----------|-------|
| v6 pipeline + classifier + integrity + c2 crypto | `src/base/messaging/tests/` | `v6_pipeline_test`, `e2e_ingest_classifier_test`, `envelope_signer_test`, `relay_history_test`, `chat_sync_test` (9 tests), `v6_integrity_test`, `e2e_relay_crypto_test` (2 tests) |
| v6 schema | `v6_schema_test` | seq/epoch/sync_state |
| SqliteThreadStore | `sqlite_thread_store_test` | AI transcript, memory |
| Messaging foundation | `tests/messaging_foundation_test.cpp` | envelope + legacy reject |

Run: `./build/tests/base_messaging_tests/pp_browser_v6_pipeline_test` (and siblings).

## Known gaps (summary)

1. **c3** — PSK export/import, fingerprint gate, rotation UX; enable **`e2e_public`** send.
2. Poll still invoked each UI frame (throttled to 2 s — D032 partial).
3. Composer maxlength not wired.
4. Live relay integration tests (D093) — client ready; coordinate against external relay.

**Non-chat safety gaps:** [platform-safety-limits](../platform-safety-limits/).
