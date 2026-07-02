# Current state — as of 2026-07-02

Inventory of what exists in the codebase today. Update this file when landing phase work.

**Planned but not implemented:** see [DESIGN.md](DESIGN.md) and D008–D068 in [DECISIONS.md](DECISIONS.md).  
**Implementation:** ready — wave 1 per [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order) (`v2a-core` ∥ e2e `c1`).

## Release scope (v1 batch)

Agents should implement **this scope** before the first customer release unless explicitly expanded:

| In scope | Out of scope (unless expanded) |
|----------|--------------------------------|
| chat v2a–v6 | post-v4, post-v6b/c/d |
| e2e c1–c3 (private `e2e` tier) | e2e c3+ (`e2e_public` auto-key), c4 PQ |
| AI storage + memory (v3) | Group E2E (O008) |

## Persistence

| Area | Status | Location |
|------|--------|----------|
| Thread index + per-thread JSON | Implemented (legacy) | `src/base/messaging/JsonThreadStore.*` |
| **SqliteThreadStore** (target v2a) | Not implemented | D028 — `thread.db` + `profile.db` (`threads` catalog + `outbox` + `chat_targets`, D035) |
| Profile-scoped paths | Implemented | [CONFIGURATION.md](../../docs/CONFIGURATION.md) — `{data_dir}/profiles/{id}/threads/` |
| `IThreadStore` interface | Implemented | `src/base/messaging/IThreadStore.h` |
| SQLite vendored | **Yes** — not linked to `pp_base` yet | `third_party/sqlite/`, `cmake/dependencies.cmake`; v2a task = link + `SqliteThreadStore` (D028) |
| `MessagingLimits.h` | Not implemented | D029 — `src/base/messaging/MessagingLimits.h` |
| AI home / new AI thread persistence | Partial | Threads created in store; see gaps below |
| Durable `ConversationSummary` on disk | Not implemented | In-memory only on `Conversation` |
| Clear history (truncate messages) | Not implemented | Only full `DeleteThread` |
| Forget memory API | Not implemented | Roadmap in [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) |
| Message / envelope size limits | **Not implemented** | No cap on compose, send, or ingest (D029) |
| Windowed transcript load | **Not implemented** | Full thread loaded every refresh (D031) |
| Agent context full-thread load | **Not implemented** | `AgentSession` calls `GetMessages` each turn (D039) |
| Compaction / summary on disk | Not implemented | No `ICompactionService` (D040) |

### On-disk layout (today — legacy)

```
{data_dir}/profiles/{profile_id}/threads/index.json
{data_dir}/profiles/{profile_id}/threads/{thread_id}.json   # flat JSON — replaced in v2a
```

**Target (D028, D035, D069, D075):**

```
threads/profile.db          # threads catalog (+ group_id) + outbox + chat_targets
threads/{thread_id}/thread.db   # messages.chat_payload BLOB, display_order, …
threads/{thread_id}/blobs/      # [future] attachments (D075)
```

No `index.json` in target layout (replaces legacy index + flat JSON).

## Data model (today)

### `Thread` — `src/base/messaging/ThreadTypes.h`

- Has `encrypted` bool — **never set to `true` anywhere** (schema + UI binding only).
- No `channel` field.

### `ThreadMessage`

- Has UUID `id`, `delivery`, `relay_visible`.
- No `content_type`, `payload`, `sender_seq`, or `session_epoch`.
- `content_rml` may be set from **unverified** relay poll — rendered without escape for peers (D030 gap).

### `RelayEnvelope` (baseline — legacy wire)

- Has `thread_id` on envelope — **removed in target** (D056/D063).
- `RelayMessageBody` uses flat `body.text` — target is `body.e2e.payload_b64` only (D090).
- See `src/base/messaging/ThreadTypes.h`, `tests/messaging_foundation_test.cpp`, `MockRelayClient` in `ServiceClientsImpl.cpp`.

### `TranscriptEntry` / `Conversation` — `src/base/ai/conversation/`

- In-memory turn-pair model for legacy AI path.
- `StartNewConversation()` clears memory only — **not wired to thread store**.
- Used when `ChatController` falls back to `agent_->Submit()` without messaging router.

## Messaging and routing

| Feature | Status | Location |
|---------|--------|----------|
| HTTP relay send + poll dedup | Implemented | `src/feature/messaging/P2pMessagingService.*` |
| Local write before send | Implemented | `SendUserMessage` appends then relays |
| `HasMessageId` dedup | Implemented (legacy: profile-global) | `JsonThreadStore` in-memory set; target: per-thread `thread.db` PK (D034) |
| Inbound signature verify | **Not implemented** (spec: [E016](../e2e-message-crypto/DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add), D081) | `Ed25519Signer::Verify` unused on poll; no `PeerSigningKeyStore` |
| `IRelayClient` history fetch | **Not implemented** | v6 — `FetchChatTargetMessages` / `GET /v1/chat-targets/messages` (D027/D058); interface today: `Send` + `PollInbox` only (`ServiceClients.h`) |
| Relay poll every UI frame | **Implemented (gap)** | `ChatController::Update` → `PollAndMerge` (D032) |
| `@ai` scoped assist | Implemented (local only) | `MessageRouter` → `SubmitScopedAssist` |
| `@ai+` / `@ai++` shared modes | **Deferred** (D012) | Design: local `@ai` only for v1 |
| Direct P2P transport | Not implemented | All outbound via `IRelayClient` |
| libp2p messaging glue | Stub | `src/libp2p/integration/host/` |
| `sender_seq` / gap detection | Not implemented | Design: D008–D011, phase v6 |
| Windowed sync (tail / scroll / gap repair) | Not implemented | Design: D009 |
| Per-peer sync state / `history_floor_seq` | Not implemented | Per `(peer, epoch)`; below floor → silent discard (D037) |

### `FindOrCreateDirectThread`

- Keys on local **`Contact.id`** only — **one direct thread per contact** (baseline code).
- Does not consider public vs E2E or communicating identity — gap vs target design.
- Target (D056, D079, D082): wire routes by `sender_contact_id` (identity **value**, e.g. `relay:abc123`) + `route.channel` → `ChatTargetKey`; local `thread_id` and `Contact.id` not on envelope.

### JsonThreadStore performance gaps (legacy)

- `EnsureLoaded()` reads **all threads** into RAM on first access.
- `AppendMessage` rewrites entire thread JSON file per message.
- No atomic write (crash can corrupt file).

## AI conversation pipeline

| Feature | Status | Location |
|---------|--------|----------|
| Sliding window context | Implemented | `SlidingWindowContextPolicy`, `ThreadContextPolicy` |
| Thread-mode agent turns | Implemented | `AgentSession::SubmitToThread` |
| Scoped assist context | Implemented | `ThreadContextPolicy::BuildAssistContext` |
| Payload fast path / turn plan | Implemented | [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) |
| `SubmitToThread` persists user + assistant to store | Implemented | `AgentSession::StartTurn`, `PersistAssistantToThread` |
| Unified transcript for AI home | **Gap** | See below |

### AI home / new chat gap

`ChatController::OnNewChat()` calls `CreateNewAiThread()` but agent turns may still use in-memory `Conversation` when routing differs.

## UI (today)

| Feature | Status | Location |
|---------|--------|----------|
| Sidebar sessions from thread index | Implemented | `ChatController::SyncShellSessions` |
| Close thread (= delete) | Implemented | `InboxController::CloseThread` |
| New chat (= new AI thread) | Implemented | `OnNewChat` → `CreateNewAiThread` |
| E2E vs public chrome | Partial | `thread_encrypted` binding; no threads use `encrypted=true` |
| Delivery / transport badges on messages | Not implemented | `BuildDisplayRows` has no delivery/transport fields |
| Clear history / forget memory menus | Not implemented | — |
| Composer maxlength | **Not set** | `assets/views/composer.rml` |

## Tests

| Area | Location | Notes |
|------|----------|-------|
| Sliding window / conversation | `src/base/ai/conversation/tests/` | |
| Messaging foundation | `tests/messaging_foundation_test.cpp` | Uses **legacy** `RelayEnvelope` — update in v2a-p2p |
| JsonThreadStore | No dedicated unit tests | v2a adds `SqliteThreadStore` tests |

### Manual E2E test plan (c2/c3)

1. Two profiles (two `data_dir` trees) or two app instances.
2. Exchange PSK via export/import (c3) or dev fixture.
3. Send via mock relay or HTTP relay; verify relay sees `body.e2e.payload_b64` only (c2).
4. Restart both sides; verify outbox + transcript (v6/c2).

## Known gaps (summary)

1. Dual transcript models (`Conversation` vs `ThreadMessage`) — converge on store.
2. No clear-history vs delete-thread distinction.
3. No durable AI memory layer on disk.
4. One direct thread per contact — no channel split.
5. No `ChatPayload` / annotation schema in C++ yet.
6. No transport provenance field or UI.
7. `Thread.encrypted` unused in creation paths.
8. No `sender_seq`, session epoch, strict ingest, durable outbox, or gap-repair sync.
9. No resource bounds on message size, poll rate, or UI window (D029–D033).
10. No agent tail context API; full `GetMessages` per turn (D039).
11. No compaction service or summary size cap (D040).
12. No outbox/gap repair numeric limits (D041); annotation cap (D042).
13. Inbound relay messages not signature-verified; no `PeerSigningKeyStore` (E016/D081); remote `content_rml` trusted (D030).
14. JsonThreadStore eager load + full-file rewrite on append.
15. Schema bumps require wipe (D016); v2a replaces JSON with SQLite (D028).
16. `@ai` local-only in v1 — shared modes deferred (D012).
17. No unified E2E backfill / user sync / peer-direct history (D058–D060) — planned v6.
18. `ThreadMessage` lacks `display_order` in C++ (D066) — planned v2a-core; legacy `RelayEnvelope` has `thread_id` — v2a-p2p cutover (D063).
19. Empty gap close guard + late fill (D067) and compromised outbox freeze (D068) — spec only, v6.
20. **Legacy wire in mocks/tests** — `MockRelayClient`, `messaging_foundation_test` use pre-D063 shape; replace in v2a-p2p.

**Non-chat safety gaps** (LLM HTTP, profile JSON stores): see [platform-safety-limits](../platform-safety-limits/).
