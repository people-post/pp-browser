# Phased roadmap

Check boxes when the work is **merged and verified**. Add sub-items freely; keep phase boundaries stable unless DECISIONS.md records a change.

---

## Baseline (pre-project) — done

Existing foundation this project builds on.

- [x] `IThreadStore` + `JsonThreadStore` (index + one file per thread) — **to be replaced by SqliteThreadStore in v2a**
- [x] `Thread` / `ThreadMessage` types and JSON serde
- [x] `P2pMessagingService` send, poll, message-id dedup
- [x] `MessageRouter` (AI thread, direct relay, `@ai` assist)
- [x] `AgentSession::SubmitToThread` + thread context policy
- [x] Sliding window / turn coordinator for AI
- [x] Sidebar: list threads, new AI thread, close (= delete) thread
- [x] Docs: [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md)

---

## Phase v2a — SQLite storage + unified transcript

**Goal:** Replace `JsonThreadStore` with **`SqliteThreadStore`** (D028); one durable path for all AI chat; clear-history API; no silent loss on restart.

### Vendor SQLite (`pp_base`)

- [ ] Add SQLite to build (amalgamation or system lib) — **not** libp2p fork SQLite
- [ ] Document in [BUILD.md](../../docs/BUILD.md) / `third_party` if vendored

### `SqliteThreadStore` (D028)

- [ ] Layout: `threads/profile.db` (`threads` + `outbox`), `threads/{id}/thread.db` — no `index.json` (D035)
- [ ] `profile.db`: `threads` catalog + `outbox` table (D034, D035)
- [ ] `thread.db`: `messages`, `memory`, `sync_state` tables (sync table schema ready; watermarks used in v6)
- [ ] Lazy-open `thread.db` per thread; WAL mode per connection
- [ ] `AppendMessage` / `UpdateMessage`: `thread.db` first; maintain `profile.db` `outbox` + `threads.updated_at` / `unread_count` (D035)
- [ ] `ListThreads`: catalog query + visible-row verify/repair against `thread.db` (D035)
- [ ] Profile open: `readdir` repair — stub `threads` row for orphan `thread.db` dirs (D035)
- [ ] `HasMessageId(thread_id, message_id)` via `thread.db` `messages.id` PK (D034)
- [ ] Change `IThreadStore::HasMessageId` signature; update `P2pMessagingService` poll path
- [ ] `ClearMessages(thread_id)` — `DELETE FROM messages`; keep memory/sync tables
- [ ] `DeleteThread` — `profile.db` txn (`threads` + `outbox`) then remove dir (D035)
- [ ] Wipe legacy `threads/index.json` and flat `threads/{id}.json` on first run (D016 — no migration)
- [ ] Wire app bootstrap to `SqliteThreadStore` instead of `JsonThreadStore`
- [ ] Unit tests: append, update delivery, per-thread dedup, clear, delete, `profile.db` catalog + outbox, lazy open, visible-row repair (D035)
- [ ] `MessagingLimits` constants (D029); reject oversize on append
- [ ] `GetMessagesPage` for UI window (D031)
- [ ] LRU cap on open `thread.db` handles (D029)

### Agent / chat integration

- [ ] Remove or gate legacy `agent_->Submit()` path — always `SubmitToThread` for AI threads
- [ ] On open AI thread: load display via `GetMessagesPage` + `BuildDisplayRows` (D031)
- [ ] `StartNewConversation()` deprecated or mapped to new thread creation only
- [ ] Persist assistant `content_rml` + `chat_actions` on thread messages

### UX

- [ ] Thread menu: **Clear history** → choice sheet (D024)
- [ ] **Close conversation** = delete thread directory + `profile.db` cleanup
- [ ] Composer maxlength aligned with `kMaxComposeTextBytes` (D029)

### Docs

- [ ] Update [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) persistence section
- [ ] Update [CONFIGURATION.md](../../docs/CONFIGURATION.md) on-disk layout
- [ ] Update this file + README progress snapshot

**Exit criteria:** New AI thread survives restart via SQLite; clear-messages level empties UI but keeps sidebar entry; per-thread `HasMessageId` rejects duplicate append in the same thread.

---

## Phase v2b — Public vs E2E channel split

**Goal:** Same contact can have two isolated direct threads.

### Model

- [ ] Add `ThreadChannel` enum (`public_relay`, `e2e`)
- [ ] `channel` on `Thread` + `profile.db` `threads`; `encrypted = (channel == e2e)`
- [ ] `FindOrCreateDirectThread(contact_id, channel)` — replace single-key lookup
- [ ] Wipe any remaining legacy JSON thread files (D016)

### Creation flows

- [ ] Contact action “Message” specifies channel (or default public until E2E exists)
- [ ] Future: “Secure message” creates `e2e` thread when crypto ready

### UI

- [ ] Sidebar **grouped sections** (D023): AI, Public, Private
- [ ] E2E shell styling when `channel=e2e`

### Memory boundary

- [ ] AI context and memory never cross channels (per `thread_id` / `thread.db`)

**Exit criteria:** Two thread dirs + DBs for one contact; sidebar shows correct group.

---

## Phase v3 — Durable AI memory and forget semantics

**Goal:** Long-running AI threads compact gracefully; user can forget without losing transcript.

### Memory storage

- [ ] `memory` table in `thread.db` (D028)
- [ ] `IThreadStore::GetThreadMemory` / `SetThreadMemory`
- [ ] Wire `SlidingWindowContextPolicy` to inject summary when present

### Compaction (minimal v3)

- [ ] `ICompactionService` — generate summary when turn count exceeds threshold
- [ ] Background or on-turn trigger; version increment on summary

### UX

- [ ] **Forget what AI learned** — `DELETE FROM memory` (or summary key), keeps `messages`
- [ ] Clear history choice sheet level “clear messages & memory” (D024)
- [ ] P2P disclosure copy on clear levels

### Docs

- [ ] Extend [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) compaction section

**Exit criteria:** 20+ turn AI thread uses summary in LLM context; forget memory clears `memory` table but leaves messages.

---

## Phase v4 — ChatPayload types and transport provenance

**Goal:** Unified message format (D026); reactions and rich cards; E2E transport badges.

### Message schema

- [ ] `content_type` + `payload` columns on `messages` table; `ChatPayload` JSON codec + **validator** (D029)
- [ ] Reject oversize payload on send; strip wire `content_rml` on ingest (D030)
- [ ] Types v1: `text`, `annotation`, `contact_card`, `crypto_tx`, `system`
- [ ] Relay `body.content` for public; envelope signing covers structured body
- [ ] `transport` column; set in send/receive paths

### LLM / display

- [ ] `ThreadContextPolicy` filters to `content_type=text` (+ selected `system`)
- [ ] `BuildDisplayRows`: merge `annotation` onto `target_message_id`; card templates
- [ ] E2E: per-message transport badge

### Protocol

- [ ] Relayed annotations/cards use same envelope as text
- [ ] Dedup by `message_id` per thread (`thread.db` PK, D034)

**Exit criteria:** Reaction survives restart; contact card renders; E2E shows relay badge on fallback.

---

## Phase v6 — Sender seq, gap detection, and windowed sync

**Goal:** Private/direct chat detects missing peer messages and syncs reliably.

**Depends on:** v2b, v4; relay `GET /v1/threads/{id}/messages` (D027) for offline gap repair.

### Schema and persistence

- [ ] `sender_seq`, `session_epoch`, `sender_contact_id` on messages + relay envelope (D021)
- [ ] `sync_state` table populated per `(peer, session_epoch)`
- [ ] `chat_targets.json` sidecar for `next_outgoing_seq`, `session_epoch`
- [ ] `GetMessagesBySeqRange` on `IThreadStore` for tail/gap/backfill
- [ ] Assign `(message_id, sender_seq)` at first local persist; serialize per chat target

### Send / receive

- [ ] Sign envelope including `sender_seq`, `session_epoch`, `sender_contact_id`
- [ ] Durable outbox: `ListPendingOutbox()` from `profile.db` on startup (D017, D028)
- [ ] Within-epoch sender contract; receive pipeline (D022, D033); ingest D013/D018
- [ ] Inbound Ed25519 verify; strip remote `content_rml` (D030)
- [ ] Poll backoff min 2 s foreground (D032); cap poll batch (D029)
- [ ] Clear history → `history_floor_seq` in `sync_state`; below-floor silent discard (D037)
- [ ] Peer reset / compromise recovery (D014, DESIGN § Compromise recovery)

### Sync modes

- [ ] **Tail sync** — `GetMessagesBySeqRange` desc limit 50
- [ ] **Gap repair** — peer direct; relay D027 when offline
- [ ] **History backfill** — scroll-up, limit 25
- [ ] Reorder buffer k=32 (D020); display sort D019

### Integrity and UX

- [ ] Compromised / gap banners; unit tests for seq, outbox, floor, epoch, reorder

### Docs

- [ ] Extend [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md): envelope limits (D029), verify pipeline
- [ ] Document single-active-device (D015)

**Exit criteria:** Gap auto-repair on direct path; outbox survives restart via `profile.db`; seq/floor/epoch tests pass.

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

### Shared reply (`@ai+`)

- [ ] On AI complete: persist + send one row — `generation=ai_on_behalf`, `relay_visible=true`, +1 sync seq

### Shared full (`@ai++`)

- [ ] Persist + send prompt row then reply row (+2 seq)

### Agent session + UX

- [ ] `PersistAssistantToThread` sets `relay_visible`, `generation`, `seq_owner` per D012
- [ ] Composer hints; confirm dialog for `@ai+` / `@ai++`

### Tests

- [ ] Parser and mode seq-count tests

**Exit criteria:** Three modes routable in direct threads; shared modes relay with correct seq count; local mode unchanged for peer.

---

## Deferred — cross-thread FTS search

**Goal:** Agent/search across all threads without loading every transcript.

- [ ] Optional FTS5 virtual table in a thread or profile search DB (not `profile.db`)
- [ ] Internal API only until UI needs it

**Trigger:** search feature request or agent tool needs full-text recall.

---

## Cross-cutting tasks

- [ ] `SqliteThreadStore` unit tests (`profile.db`, per-thread db, clear, delete, outbox, **size reject**)
- [ ] Ingest tests: oversize envelope, remote content_rml stripped (D029–D030)
- [ ] Agent tool docs if `list_conversations` must expose channel
- [ ] Fuzz/dedup: duplicate relay `message_id` ignored via per-thread store check
- [ ] Sender seq tests (v6); `@ai` mode tests (v6b)

---

## Changelog

| Date | Change |
|------|--------|
| 2026-06-27 | Project doc created from planning discussion |
| 2026-06-27 | D008–D011: sender seq, windowed sync, seq lifecycle, session compromise; phase v6 |
| 2026-06-27 | D012: three `@ai` modes; phase v6b; sync seq only when `relay_visible` |
| 2026-06-29 | D013–D014: strict normal-or-compromised ingest, peer reset = epoch bump; DESIGN P2P sync rewrite |
| 2026-06-29 | D015–D022: single active device, no migration, durable outbox, public ingest, display order, reorder window, sender_contact_id, receive pipeline |
| 2026-06-29 | D023–D027: sidebar groups, clear choice sheet, ChatPayload, relay API |
| 2026-06-29 | D034: per-thread message_id dedup; profile.db outbox-only |
| 2026-06-29 | D028: SQLite per thread + profile.db from v2a; drop JSON message stage; v5 absorbed |
| 2026-06-29 | D035: drop `index.json`; sidebar catalog in `profile.db` `threads`; lazy visible-row verify |
| 2026-06-29 | D036: rename `registry.db` → `profile.db` |
| 2026-06-29 | D037: clear history floor = sync exclusion + silent discard; amend D010/D013 |
