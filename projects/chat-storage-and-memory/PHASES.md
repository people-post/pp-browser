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

## Phase v6 — Sender seq, gap detection, and windowed sync

**Goal:** Private/direct chat detects missing peer messages and syncs reliably without full-history pull.

**Depends on:** v2b (channel split), v4 (`transport`, envelope extensions); peer backfill requires direct/libp2p transport (stub today).

### Schema and persistence

- [ ] Add `sender_seq`, `session_epoch` to `ThreadMessage` and relay envelope
- [ ] Persist `next_outgoing_seq`, `session_epoch` on chat target `(contact_id, channel)`
- [ ] Per-thread sync state: `contiguous_peer_seq`, `loaded_min/max_seq`, `history_floor_seq` **per `(peer, session_epoch)`**, `sync_state`
- [ ] Assign `(message_id, sender_seq)` at first local persist; increment seq only when `relay_visible=true`
- [ ] Failed send retry: same `message_id` + `sender_seq` (D010)

### Send / receive

- [ ] Sign envelope including `sender_seq` and `session_epoch`
- [ ] Implement within-epoch sender contract (DESIGN.md § Within-epoch sender contract)
- [ ] Ingest classifier: normal · gap · compromised (D013)
- [ ] Distinguish bootstrap (high seq / new epoch) vs contiguous gap (D009)
- [ ] On clear history: set `history_floor_seq[peer][epoch]`; floor violation → compromised (D013)
- [ ] Peer reset: bump `session_epoch`, optional `epoch_start` control row (D014)

### Sync modes

- [ ] **Tail sync** on thread open / reconnect / new device (default N=50)
- [ ] **Gap repair** — auto-request missing range from peer (direct) with relay fallback
- [ ] **History backfill** — scroll-to-top triggers page fetch (25 messages)
- [ ] Reorder buffer (short window) before declaring gap

### Integrity and UX

- [ ] Full compromised triggers per D013 (seq conflict D011, floor violation, epoch decrease, rewind, repair failure)
- [ ] Key rotation + new secure chat flow; bump `session_epoch`, reset seq for new epoch only (D014)
- [ ] Gap banner in chat UI; scroll hint when older history exists
- [ ] Unit tests: seq assignment, retry idempotency, gap detect, bootstrap vs gap, clear-history floor → compromised, epoch bump fresh stream, same-epoch seq=1 rewind → compromised

### Docs

- [ ] Extend [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md) relay envelope when implemented
- [ ] Update this file + README progress snapshot

**Exit criteria:** Simulated gap in tail triggers auto-repair; new epoch accepts seq=1 bootstrap; clear history then `sender_seq ≤ floor` triggers compromised; same-epoch rewind triggers compromised; conflict triggers compromise UX.

---

## Phase v6b — `@ai` three modes (local, shared reply, shared full)

**Goal:** Direct-thread AI assist with explicit local vs shared-to-peer modes; shared modes use trigger user’s sync seq (D012).

**Depends on:** v6 (`sender_seq`, send pipeline); v4 (`transport` badges) for shared-row UX.

### Parser and routing

- [ ] Extend `AtAiParseResult` with `AtAiMode`: `Local`, `SharedReply`, `SharedFull`
- [ ] Parse `@ai`, `@ai+`, `@ai++` (and optional long-form aliases)
- [ ] `MessageRouter` branches per mode; pass mode into `SubmitScopedAssist`

### Local mode (`@ai`)

- [ ] Persist AI row: `relay_visible=false`, `sender_contact_id=ai:assistant`, no `sender_seq`, `ai_invoke_mode=local`
- [ ] Matches today’s behavior; no relay

### Shared reply (`@ai+`)

- [ ] On AI complete: persist + send one row — `generation=ai_on_behalf`, `relay_visible=true`, +1 sync seq
- [ ] Prompt not relayed (optional local-only note, not required)
- [ ] Wire as trigger user’s stream; local UI assistant bubble + shared badge

### Shared full (`@ai++`)

- [ ] Persist + send prompt row first — stripped text, `generation=user`, seq N
- [ ] On AI complete: persist + send reply — `generation=ai_on_behalf`, seq N+1
- [ ] Independent retry per row (same id+seq on failure)

### Agent session

- [ ] `AgentTurnMode` or flag for scoped assist mode (local / shared_reply / shared_full)
- [ ] `PersistAssistantToThread` sets `relay_visible`, `generation`, `seq_owner` per D012
- [ ] Shared paths invoke `P2pMessagingService` send after persist

### UX

- [ ] Composer placeholder / hints for three modes
- [ ] Confirm dialog: `@ai+` vs `@ai++` copy; E2E transport badge on shared rows

### Tests

- [ ] Parser: `@ai`, `@ai+`, `@ai++`, aliases
- [ ] Local: no seq, not relayed
- [ ] Shared reply: one seq, one envelope
- [ ] Shared full: two seq, two envelopes, stripped prompt body

### Docs

- [ ] Update [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md) `@ai` section when implemented

**Exit criteria:** Three modes routable in direct threads; shared modes relay with correct seq count; local mode unchanged for peer.

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
- [ ] Sender seq tests: gap repair, bootstrap tail, clear-history floor → compromised, epoch bump, same-epoch rewind (v6)
- [ ] `@ai` mode tests: local no-seq, shared reply +1, shared full +2 (v6b)

---

## Changelog

| Date | Change |
|------|--------|
| 2026-06-27 | Project doc created from planning discussion |
| 2026-06-27 | D008–D011: sender seq, windowed sync, seq lifecycle, session compromise; phase v6 |
| 2026-06-27 | D012: three `@ai` modes; phase v6b; sync seq only when `relay_visible` |
| 2026-06-29 | D013–D014: strict normal-or-compromised ingest, peer reset = epoch bump; DESIGN P2P sync rewrite |
