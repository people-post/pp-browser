# Current state — as of 2026-06-27

Inventory of what exists in the codebase today. Update this file when landing phase work.

## Persistence

| Area | Status | Location |
|------|--------|----------|
| Thread index + per-thread JSON | Implemented | `src/base/messaging/JsonThreadStore.*` |
| Profile-scoped paths | Implemented | [CONFIGURATION.md](../../docs/CONFIGURATION.md) — `{data_dir}/profiles/{id}/threads/` |
| `IThreadStore` interface | Implemented | `src/base/messaging/IThreadStore.h` |
| SQLite chat store | Not implemented | libp2p fork has optional SQLite; unrelated to app chat |
| AI home / new AI thread persistence | Partial | Threads created in store; see gaps below |
| Durable `ConversationSummary` on disk | Not implemented | In-memory only on `Conversation` |
| Clear history (truncate messages) | Not implemented | Only full `DeleteThread` |
| Forget memory API | Not implemented | Roadmap in [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) |

### On-disk layout (today)

```
{data_dir}/profiles/{profile_id}/threads/index.json
{data_dir}/profiles/{profile_id}/threads/{thread_id}.json   # { "messages": [...] }
```

## Data model (today)

### `Thread` — `src/base/messaging/ThreadTypes.h`

- Has `encrypted` bool — **never set to `true` anywhere** (schema + UI binding only).
- No `channel` field.

### `ThreadMessage`

- Has UUID `id`, `delivery`, `relay_visible`.
- No `kind`, `transport`, `target_message_id`, or `user_payload`.
- AI thread turns store `text` + optional `content_rml` + `chat_actions`.

### `TranscriptEntry` / `Conversation` — `src/base/ai/conversation/`

- In-memory turn-pair model for legacy AI path.
- `StartNewConversation()` clears memory only — **not wired to thread store**.
- Used when `ChatController` falls back to `agent_->Submit()` without messaging router.

## Messaging and routing

| Feature | Status | Location |
|---------|--------|----------|
| HTTP relay send + poll dedup | Implemented | `src/feature/messaging/P2pMessagingService.*` |
| Local write before send | Implemented | `SendUserMessage` appends then relays |
| `HasMessageId` global dedup | Implemented | `JsonThreadStore` + poll merge |
| `@ai` scoped assist | Implemented | `MessageRouter` → `SubmitScopedAssist` |
| Direct P2P transport | Not implemented | All outbound via `IRelayClient` |
| libp2p messaging glue | Stub | `src/libp2p/integration/host/` |

### `FindOrCreateDirectThread`

- Keys on `contact_id` only — **one direct thread per contact**.
- Does not consider public vs E2E — gap vs target design.

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

`ChatController::OnNewChat()` calls `CreateNewAiThread()` (empty thread in store) but agent turns may still use in-memory `Conversation` when routing differs. Messaging-ready path uses `MessageRouter` → `SubmitToThread` for AI threads — **verify** all code paths persist to the active thread file.

Legacy path: `agent_->Submit()` uses `Conversation` only — no disk.

## UI (today)

| Feature | Status | Location |
|---------|--------|----------|
| Sidebar sessions from thread index | Implemented | `ChatController::SyncShellSessions` |
| Close thread (= delete) | Implemented | `InboxController::CloseThread` |
| New chat (= new AI thread) | Implemented | `OnNewChat` → `CreateNewAiThread` |
| E2E vs public chrome | Partial | `thread_encrypted` binding; no threads use `encrypted=true` |
| Delivery / transport badges on messages | Not implemented | `BuildDisplayRows` has no delivery/transport fields |
| Clear history / forget memory menus | Not implemented | — |

## Tests

| Area | Location |
|------|----------|
| Sliding window / conversation | `src/base/ai/conversation/tests/` |
| Messaging foundation | `tests/messaging_foundation_test.cpp` |
| JsonThreadStore | No dedicated unit tests found |

## Known gaps (summary)

1. Dual transcript models (`Conversation` vs `ThreadMessage`) — converge on store.
2. No clear-history vs delete-thread distinction.
3. No durable AI memory layer on disk.
4. One direct thread per contact — no channel split.
5. No annotation / meta-message schema.
6. No transport provenance field or UI.
7. `Thread.encrypted` unused in creation paths.
