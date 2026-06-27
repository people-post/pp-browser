# Phased roadmap

Check boxes when the work is **merged and verified**. Add sub-items freely; keep phase boundaries stable unless DECISIONS.md records a change.

---

## Baseline (pre-project) — done

Existing foundation this project builds on.

- [x] `IThreadStore` + `JsonThreadStore` (index + one file per thread)
- [x] `Thread` / `ThreadMessage` types and JSON serde
- [x] `P2pMessagingService` send, poll, message-id dedup
- [x] `MessageRouter` (AI thread, direct relay, `@ai` assist)
- [x] `AgentSession::SubmitToThread` + thread context policy
- [x] Sliding window / turn coordinator for AI
- [x] Sidebar: list threads, new AI thread, close (= delete) thread
- [x] Docs: [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md)

---

## Phase v2a — Persistence polish and unified transcript

**Goal:** One durable path for all AI chat; explicit clear-history API; no silent loss on restart.

### Store API and JSON schema

- [ ] Add `ClearMessages(thread_id)` to `IThreadStore` + `JsonThreadStore`
- [ ] Bump thread/message JSON `schema_version` if fields added; document in CONFIGURATION or project DECISIONS
- [ ] Optional: persist `user_payload` on `ThreadMessage` for AI form submits in threads
- [ ] Unit tests for `ClearMessages`, append-after-clear, index unchanged

### Agent / chat integration

- [ ] Remove or gate legacy `agent_->Submit()` path when messaging is ready — always `SubmitToThread` for AI threads
- [ ] On open AI thread: load display rows from store (already via `BuildDisplayRows`); ensure new threads start empty on disk
- [ ] `StartNewConversation()` either deprecated for threaded mode or mapped to new thread creation only
- [ ] Persist assistant `content_rml` + `chat_actions` on thread messages after parse (if not already complete)

### UX

- [ ] Thread menu: **Clear history** (truncate messages, keep thread id/title)
- [ ] Keep **Close conversation** = existing delete
- [ ] Confirm dialog copy for clear vs delete

### Docs

- [ ] Update [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) v2 persistence section when v2a ships
- [ ] Update this file + README progress snapshot

**Exit criteria:** New AI thread survives app restart with full transcript; clear history empties UI but keeps sidebar entry.

---

## Phase v2b — Public vs E2E channel split

**Goal:** Same contact can have two isolated direct threads.

### Model

- [ ] Add `ThreadChannel` enum (`public_relay`, `e2e`) — name TBD in implementation
- [ ] JSON field on `Thread`; `encrypted = (channel == e2e)` for existing UI binding
- [ ] `FindOrCreateDirectThread(contact_id, channel)` — replace single-key lookup
- [ ] Migration: existing direct threads default to `public_relay`

### Creation flows

- [ ] Contact action “Message” specifies channel (or default public until E2E exists)
- [ ] Future: “Secure message” creates `e2e` thread when crypto ready

### UI

- [ ] Sidebar shows channel distinction (two rows or badge — resolve open question in README)
- [ ] E2E shell styling activates when `encrypted=true`

### Memory boundary

- [ ] Document: AI context and future memory never cross channels (enforce in thread-scoped store keys)

**Exit criteria:** Two thread files can exist for one contact; opening each shows separate histories.

---

## Phase v3 — Durable AI memory and forget semantics

**Goal:** Long-running AI threads compact gracefully; user can forget without losing transcript.

### Memory storage

- [ ] Persist `ConversationSummary` per thread (JSON sidecar or top-level in thread file)
- [ ] `IThreadStore::GetThreadMemory` / `SetThreadMemory`
- [ ] Wire `SlidingWindowContextPolicy` / thread policy to inject summary when present

### Compaction (minimal v3)

- [ ] `ICompactionService` interface (or inline in turn complete) — generate summary when turn count exceeds threshold
- [ ] Background or on-turn trigger; version increment on summary

### UX

- [ ] **Forget what AI learned** — clears summary/facts, keeps messages
- [ ] **Clear visible history** — optional checkbox “Also forget AI memory”
- [ ] Copy explaining local-only vs peer retention for P2P threads

### Docs

- [ ] Extend [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) compaction section

**Exit criteria:** 20+ turn AI thread uses summary in LLM context; forget memory clears summary but leaves bubbles.

---

## Phase v4 — Annotations and transport provenance

**Goal:** Stable IDs for reactions; E2E users see relay vs direct.

### Message schema

- [ ] `MessageKind`: content | annotation | system
- [ ] `target_message_id`, `annotation_type` for likes/edits/receipts
- [ ] `MessageTransport`: local | relay | direct
- [ ] Set `transport` in send/receive paths

### LLM / display

- [ ] `ThreadContextPolicy` filters non-content messages
- [ ] `BuildDisplayRows` merges annotations onto targets (or inline system rows)
- [ ] E2E: per-message transport badge (RCSS + data model fields)

### Protocol (if syncing annotations)

- [ ] Relay envelope extension for annotation messages (or local-only v1)
- [ ] Dedup still by `message_id`

**Exit criteria:** Like on message survives restart; E2E thread shows relay badge on fallback sends.

---

## Phase v5 — Optional SQLite backend (deferred)

**Goal:** Scale and query without changing feature code.

- [ ] `SqliteThreadStore` implementing `IThreadStore`
- [ ] Config flag or auto-migrate from JSON
- [ ] Cross-thread search API (internal; UI optional)

**Trigger to start:** annotation volume, search feature, or JSON rewrite performance issues.

---

## Cross-cutting tasks

- [ ] Add `JsonThreadStore` unit tests (load, append, delete, clear, dedup)
- [ ] Agent tool docs if `list_conversations` must expose channel
- [ ] Fuzz/dedup test: duplicate relay `message_id` ignored

---

## Changelog

| Date | Change |
|------|--------|
| 2026-06-27 | Project doc created from planning discussion |
