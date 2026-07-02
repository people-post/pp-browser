# Current state — as of 2026-07-02

Inventory of what exists in the codebase today. Update this file when landing phase work.

**Planned but not implemented:** see [DESIGN.md](DESIGN.md) and D008–D068 in [DECISIONS.md](DECISIONS.md).  
**Agent batch:** Waves **1–2** merged; **Wave 3** landed; **Wave 4 v6-schema** landed. Next: **v6-pipeline** — see [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order).

## Next agent — start here

| Priority | Work | Blocked by |
|----------|------|------------|
| **Wave 4b** | **v6-pipeline** — receive classifier, ReplayWindow, history floor on clear, EnvelopeSigner hook | v6-schema (done) |
| **Wave 4c** | **v6-sync** — `FetchChatTargetMessages`, tail/gap repair | v6-pipeline |
| **Wave 5–6** | [e2e c2](../e2e-message-crypto/PHASES.md#phase-c2--messaging-integration) — AEAD on wire | v6 envelope + ingest pipeline |
| **UX gaps (v2a-core)** | Composer `maxlength` (`kMaxComposeTextBytes`) | — |

**Key paths (wave 2):**

- Types / wire: `src/base/messaging/ThreadTypes.h`, `MessagingJson.cpp` (`ParseRelayEnvelope`), `RelayWirePayload.*`, `DirectChatTarget.*`
- Store: `SqliteThreadStore.*` — `chat_targets`, `outbox`, `FindOrCreateDirectThread`, `ReconcileOutbox`, `AllocateSenderSeq`
- Feature: `P2pMessagingService.cpp`, `InboxController.cpp`, `ContactActionDispatcher.cpp`, `ContactsController.cpp`, `ChatController.cpp` (tier badges, `compose_disabled`)
- Tests: `src/base/messaging/tests/p2p_relay_wire_test.cpp`, `sqlite_thread_store_test.cpp`, `tests/messaging_foundation_test.cpp`

**Interim behaviors (do not “fix” without reading DECISIONS):**

- Relay body uses **`body.e2e.payload_b64`** but payload is **plaintext ChatPayload bytes** (base64) until e2e **c2** encrypts.
- Outbound signing still uses **JSON dump minus `signature`** — not E014 canonical bytes yet (`EnvelopeSigner` = c2).
- **`e2e_public`** threads exist and show tier badge; **compose/send disabled** until c3 auto-key.
- Inbound poll: **find-only** via `chat_targets` (no auto-create thread on unknown sender — D062).

## Release scope (v1 batch)

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| chat v2a–v6 | post-v4, post-v6b/c/d |
| e2e c1–c3 (private `e2e` tier) | e2e c3+ (`e2e_public` auto-key), c4 PQ |
| AI storage + memory (v3) | Group E2E (O008) |

## Persistence

| Area | Status | Location |
|------|--------|----------|
| **SqliteThreadStore** (v2a) | **Implemented** | `src/base/messaging/SqliteThreadStore.*` |
| Legacy `JsonThreadStore` | Retained for tests | `src/base/messaging/JsonThreadStore.*` |
| Profile-scoped paths | Implemented | `{data_dir}/profiles/{id}/threads/` — [CONFIGURATION.md](../../docs/CONFIGURATION.md) |
| `IThreadStore` interface | Extended (routing, outbox) | `src/base/messaging/IThreadStore.h` |
| SQLite + libsodium on `pp_base` | **Linked** | `src/base/CMakeLists.txt` |
| `MessagingLimits.h` | **Implemented** | `src/base/messaging/MessagingLimits.h` — append cap enforced |
| `MessagingHub` bootstrap | **SqliteThreadStore** + outbox reconcile on init | `MessagingHub.cpp` |
| Durable `ConversationSummary` on disk | **Implemented** (v3) | `IThreadStore::GetThreadMemory` / `SetThreadMemory`, `ConversationSummaryCodec`, `thread.db` `memory` key `summary` |
| Clear history UX | **Implemented** — chat header + confirm dialog; optional forget-AI checkbox on AI threads | v3 UX |
| Forget memory API | **Implemented** (`ClearMessagesOptions.forget_memory` + **Forget AI memory** menu) | v3 UX |
| Windowed transcript load | **Partial** — `GetMessagesPage` in store + inbox; UI still loads default page size | v2a-core mostly done |
| Agent context | **`GetMessagesForContext`** + **summary injection** via `ThreadContextPolicy` | `AgentSession` loads `GetThreadMemory` |
| Compaction / summary on disk | **Implemented** (async) | `ThreadCompactionService` — triggers after AI thread turns |

### On-disk layout (today — SQLite)

```
{data_dir}/profiles/{profile_id}/threads/profile.db     # threads catalog, outbox, chat_targets
{data_dir}/profiles/{profile_id}/threads/{thread_id}/thread.db
{data_dir}/profiles/{profile_id}/threads/{thread_id}/blobs/   # placeholder dir
```

Legacy `index.json` + `{thread_id}.json` are **wiped on first SqliteThreadStore open** (D016).

## Data model (today)

### `Thread` — `src/base/messaging/ThreadTypes.h`

- **`ThreadChannel`**: `None`, `E2e`, `E2ePublic` on `Thread` + `profile.db` `threads.channel`
- **`encrypted`** set when `channel ∈ { e2e, e2e_public }`
- **`peer_identity_kind` / `peer_identity_value`** on direct threads (communicating identity for routing)
- **`DirectChatTarget`** — lookup key for `chat_targets` (kind + value + channel)

### `ThreadMessage`

- Has **`display_order`**, `ChatContentType`, ChatPayload BLOB via **`ChatPayloadCodec`**
- **`transport`** column persisted (`local` / `relay` / `direct`) — set on send/receive paths (D051)
- **`sender_seq` / `session_epoch`** on E2E relay-visible rows (v6-schema); envelope uses `chat_targets.session_epoch`
- **`sync_state`** initialized per `(peer, session_epoch)` on E2E direct thread create; `GetPeerSyncState` / `SetPeerSyncState`
- **`GetMessagesBySeqRange`** for tail/gap queries (v6-schema)

### `RelayEnvelope` (v1 wire — D063/D090)

- **`envelope_version`**, `sender_contact_id`, `route.channel`, **`body.e2e.payload_b64`**
- **No `thread_id`** — `ParseRelayEnvelope` rejects legacy shapes
- **`ChatHistoryRequest` / `ChatHistoryResponse`** C++ structs + JSON serde (D072); **no HTTP/libp2p fetch yet** (v6)

### `TranscriptEntry` / `Conversation`

- In-memory turn-pair model still used on some AI fallback paths
- `StartNewConversation()` not fully deprecated

## Messaging and routing

| Feature | Status | Location |
|---------|--------|----------|
| Relay send + poll (v1 envelope) | **Implemented** | `P2pMessagingService.*` |
| Plaintext payload in `payload_b64` (pre-c2) | **Implemented** | `RelayWirePayload.*` |
| Local write before send + outbox row | **Implemented** | `AppendMessage` + `UpsertOutboxRow` |
| **`HasMessageId(thread_id, …)`** per thread | **Implemented** | `SqliteThreadStore`, poll path |
| **`FindOrCreateDirectThread(DirectChatTarget)`** | **Outbound create** | `InboxController`, `SqliteThreadStore` |
| Inbound poll routing | **Find-only** via `chat_targets` | `P2pMessagingService::PollAndMerge` |
| Startup **outbox reconciliation** | **Implemented** | `ReconcileOutbox` on hub init |
| **`AllocateSenderSeq`** | **Implemented** | `chat_targets.next_outgoing_seq` |
| **`DeleteThread` direct** | Removes thread files; **keeps `chat_targets` row** (clears `local_thread_id`) | D056 |
| Inbound signature verify | **Not implemented** | E016 / c2 |
| `IRelayClient` history fetch | **Not implemented** | v6 — `FetchChatTargetMessages` |
| Relay poll every UI frame | Still runs each frame | `ChatController::Update` → `PollAndMerge` (D032) |
| `@ai` scoped assist | Implemented | `MessageRouter` |
| libp2p messaging glue | Stub | `src/libp2p/integration/host/` |
| Gap detection / sync | Not implemented | v6-pipeline / v6-sync |

## UI (today)

| Feature | Status | Location |
|---------|--------|----------|
| Sidebar sessions + **Private/Public tier badge** | **Implemented** | `sidebar.rml`, `ChatController::SyncShellSessions` |
| Contacts: **Secure message** (`e2e`) vs **Message** (`e2e_public`) | **Implemented** | `contacts.rml`, `ContactsController` |
| **`e2e_public` compose disabled** | **Implemented** | `compose_disabled` in `composer.rml` |
| E2E chrome (`.prompt-composer--e2e`) | Partial | `thread_encrypted` binding |
| Clear history / forget memory menus | **Implemented** | `chat.rml`, `ChatController`, `InboxController` |
| Composer maxlength | **Not wired** | `kMaxComposeTextBytes` exists; `composer.rml` has no maxlength |
| Delivery / transport badges on messages | Not implemented | v4 / post-v6d |

## Tests

| Area | Location | Notes |
|------|----------|-------|
| SqliteThreadStore + ChatPayload | `src/base/messaging/tests/` | `sqlite_thread_store_test`, `p2p_relay_wire_test`, `chat_payload_validator_test`, **`v6_schema_test`** |
| Messaging foundation | `tests/messaging_foundation_test.cpp` | **v1 envelope** + legacy reject |
| Mock relay | `ServiceClientsImpl.cpp` | v1 envelope + mock reply via `SetNextReplySenderId` |
| JsonThreadStore | In-memory only | Used by foundation test |

Run: `./build/tests/base_messaging_tests/pp_browser_p2p_relay_wire_test` (and siblings).

## Known gaps (summary)

1. **Wave 4b (v6-pipeline)** — receive classifier, ReplayWindow, history floor on clear, inbound verify hook.
2. **Wave 4c (v6-sync)** — `FetchChatTargetMessages`, tail/gap repair, integrity UX.
3. **c2** — real AEAD in `payload_b64`; E014 `EnvelopeSigner`; inbound verify.
5. Canonical sign bytes (interim JSON signing on outbound).
6. Inbound relay: no Ed25519 verify; no `PeerSigningKeyStore`.
7. Poll rate / UI windowing polish (D032/D031).
8. Memory boundary across tiers — **structural** (separate `thread.db` per tier) but no explicit guard tests.

**Non-chat safety gaps:** [platform-safety-limits](../platform-safety-limits/).
