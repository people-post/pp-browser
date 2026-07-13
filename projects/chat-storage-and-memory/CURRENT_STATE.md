# Current state — as of 2026-07-06

Inventory of what exists in the codebase today. Update this file when landing phase work.

**Planned but not implemented:** see [DESIGN.md](DESIGN.md) and D008–D068 in [DECISIONS.md](DECISIONS.md).  
**Agent batch:** Waves **1–7** landed (Bucket B / D092) — v2a–v6, e2e c1–c3, post-v4, post-v6b/c/d. Next: **c3+** (`e2e_public` auto-key) unless scope expands.

## Next agent — start here

| Priority | Work | Blocked by |
|----------|------|------------|
| **c3+ / post-v1** | `e2e_public` auto-key (E013/E024) | c3 (done) |
| **Release hygiene** | Version bump, packaged smoke, tag, CI | human |

**Key paths (wave 7 — post-v1 polish):**

- Rich ChatPayload: `ChatPayloadTypes.*`, `ChatPayloadCodec.*`, `SqliteThreadStore` extended columns
- Transport badges: `MessagingJson::MessageTransportBadgeLabel`, `InboxController::BuildDisplayRows`, `chat.rml`
- Scroll backfill: `ChatSyncService::ScrollBackfill`, `ChatController::OnLoadOlderHistory`
- Shared `@ai`: `AtAiParser.*`, `MessageRouter`, `AgentSession` shared modes
- Tests: `chat_payload_rich_types_test`, `messaging_cross_cutting_test`, `relay_live_integration_test` (env-gated)

**Interim behaviors (do not “fix” without reading DECISIONS):**

- **`e2e`** relay body is **AEAD ciphertext** in `payload_b64` (c2 landed).
- **`e2e_public`** threads exist and show tier badge; **compose/send disabled** until c3 auto-key; wire stays plaintext.
- Inbound poll: **find-only** via `chat_targets` (no auto-create thread on unknown sender — D062).
- libp2p peer-direct history (D060): **`Libp2pChatHistoryService`** on `/pp-browser/chat-history/1.0.0`.
- Relay history: **`HttpRelayClient::FetchChatHistory`**; mock when `base_url` unset; live integration via env (D093).

## Release scope (v1 batch)

**Bucket B** ([D092](DECISIONS.md#d092--release-scope-bucket-b)) — **feature-complete** pending release hygiene:

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
| `IThreadStore` interface | Extended (routing, outbox, rich cols) | `src/base/messaging/IThreadStore.h` |
| SQLite + libsodium on `pp_base` | **Linked** | `src/base/CMakeLists.txt` |
| `MessagingLimits.h` | **Implemented** | poll/gap/outbox caps (D029/D041) |
| Durable `ConversationSummary` on disk | **Implemented** (v3) | `GetThreadMemory` / `SetThreadMemory` |
| Clear history + **history floor** (D037) | **Implemented** | `ClearMessages` sets `history_floor_seq = loaded_max_seq` |
| Windowed transcript load | **Implemented** | `GetMessagesPage`; UI default page size |
| Agent context + compaction | **Implemented** (v3) | `ThreadCompactionService`, summary injection |
| Rich payload columns (post-v4) | **Implemented** | `payload_json`, `target_message_id`, `generation`, etc. |

## Data model (today)

### `Thread` — `src/base/messaging/ThreadTypes.h`

- **`ThreadChannel`**: `None`, `E2e`, `E2ePublic`
- **`peer_identity_kind` / `peer_identity_value`** on direct threads
- **`DirectChatTarget`** — lookup key for `chat_targets`

### `ThreadMessage`

- **`sender_seq` / `session_epoch`** on E2E relay-visible rows (v6-schema)
- **`transport`** column (`local` / `relay` / `direct`) — badge UI in post-v6d
- **`content_type`** — text, system, annotation, contact_card, crypto_tx (post-v4)
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
| **Inbound Ed25519 verify** | **Implemented** (fail closed if key missing) | `PeerSigningKeyStore`, `RelayDirectorySigningKeyResolver` |
| **AEAD payload in `payload_b64` (`e2e`)** | **Implemented** (c2) | `E2eRelayPayloadCodec.*` |
| **`e2e_public` plaintext on wire** | **Implemented** (until c3) | `RelayWirePayload.*` |
| Local write + outbox | **Implemented** | `AppendMessage`, `ReconcileOutbox` |
| Inbound find-only routing (D062) | **Implemented** | `RelayReceivePipeline` |
| **History floor on clear** (D037) | **Implemented** | `SqliteThreadStore::ClearMessages` |
| **Poll backoff 2 s** + batch cap (D032/D029) | **Implemented** | `P2pMessagingService` |
| **`FetchChatTargetMessages`** / tail sync | **Implemented** | `ChatSyncService` |
| **User-initiated sync UX** (D059) | **Implemented** | `chat.rml` — Sync with peer / Retry sync banner |
| **Scroll backfill UX** (D052/post-v6c) | **Implemented** | Load older messages banner, `ScrollBackfill` |
| **Tail sync on open + reconnect** | **Implemented** | `OnSelectThread`, `SetRelayClient` |
| libp2p history (D060) | **Implemented** | `Libp2pChatHistoryService`, `ChatHistoryResponder` |
| Relay history fetch (D027) | **Implemented** | `HttpRelayClient`; D093 live test env-gated |
| Integrity banners (D068) | **Implemented** | compromised banner; `EpochBumpCoordinator` |
| Shared `@ai` relay (post-v6b) | **Implemented** | `AtAiParser`, `MessageRouter`, confirm UX |
| Gap detection on live ingest | **Partial** | classifier + auto `RepairGap` on `AcceptGap` |

## UI (today)

| Feature | Status | Location |
|---------|--------|----------|
| Sidebar tier badges | **Implemented** | `sidebar.rml`, `ChatController` |
| **`e2e_public` compose disabled** | **Implemented** | `compose_disabled` |
| Clear history / forget memory | **Implemented** | `chat.rml`, `InboxController` |
| Composer maxlength | **Implemented** | `composer.rml` — `kMaxComposeTextBytes` |
| Delivery / transport badges | **Implemented** | post-v6d — `chat.rml`, `components.rcss` |
| Rich payload display | **Implemented** | annotations, contact cards, crypto tx rows |
| PSK export/import/verify (c3) | **Implemented** | `chat.rml`, `PskSessionCoordinator` |

## Tests

| Area | Location | Notes |
|------|----------|-------|
| v6 pipeline + classifier + integrity | `src/base/messaging/tests/` | `v6_pipeline_test`, `e2e_ingest_classifier_test`, `v6_integrity_test` |
| Relay encrypt/decrypt + pipeline | `src/feature/messaging/tests/` | `e2e_relay_crypto_test` |
| Sync + gap + scroll + compromised | `src/feature/messaging/tests/` (`chat_sync_test`) | **13 tests** |
| Cross-cutting ingest/dedup/routing | `src/feature/messaging/tests/` (`messaging_cross_cutting_test`) | dedup, oversize, find-only, tier paths |
| Rich ChatPayload | `chat_payload_rich_types_test`, `chat_payload_validator_test` | |
| Live relay (D093) | `relay_live_integration_test` | skipped unless env set |
| Relay history mock | `relay_history_test` | |

Run: `ctest --test-dir build -R pp_browser_`

## Known gaps (summary)

1. **c3+** — `e2e_public` auto-key send path.
2. Poll still invoked each UI frame (throttled to 2 s — D032 partial).
3. Gap repair `display_order` placement (D065 partial).
4. Release hygiene — version bump, packaged smoke, tag.

**Non-chat safety gaps:** [platform-safety-limits](../platform-safety-limits/).
