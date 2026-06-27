# Decisions log

Record significant choices here so future sessions (human or agent) do not re-litigate them. Format: **ID**, **date**, **decision**, **rationale**, **alternatives considered**.

---

## D001 — JSON per-thread files for v2 (not SQLite)

**Date:** 2026-06-27  
**Decision:** Keep `JsonThreadStore` as the default persistence backend through v2–v4. Introduce SQLite only as an optional `IThreadStore` implementation (v5) when query or volume demands it.  
**Rationale:** Matches current layout and docs; simple delete/clear semantics; `IThreadStore` already abstracts storage; avoids migration cost while model is still evolving.  
**Alternatives:** SQLite from v2; single monolithic JSON file for all threads.

---

## D002 — One file per thread + index

**Date:** 2026-06-27  
**Decision:** Retain `threads/index.json` for sidebar metadata and `threads/{thread_id}.json` for message arrays.  
**Rationale:** Already implemented; clear thread = delete one file; index stays small for fast sidebar load.  
**Alternatives:** Single database or single JSON blob for all conversations.

---

## D003 — Three-layer clear semantics

**Date:** 2026-06-27  
**Decision:** Separate user actions for (1) clear visible transcript, (2) forget AI memory, (3) delete conversation / new chat. Transcript and memory can be cleared independently.  
**Rationale:** Users expect “clear chat” ≠ “make AI forget”; long threads need memory that survives UI scroll trim.  
**Alternatives:** Single “clear” that wipes everything always.

---

## D004 — Separate threads for public vs E2E per contact

**Date:** 2026-06-27  
**Decision:** Thread identity for direct chats is `(contact_id, channel)`, not contact alone. Two thread records (and files) for the same person when both modes exist.  
**Rationale:** Different crypto, transport, privacy UI, and AI memory boundaries; avoids mode-switch bugs in one transcript.  
**Alternatives:** One thread with per-message encryption flags; single thread with “mode switch” rewriting history.

---

## D005 — UUID message IDs + annotation rows

**Date:** 2026-06-27  
**Decision:** Every message has a stable UUID. Reactions and decorations are separate messages with `kind=annotation` and `target_message_id`, not mutations of the target row.  
**Rationale:** Relay dedup already uses `message_id`; append-only simplifies sync and audit; LLM context excludes annotations by kind.  
**Alternatives:** Embedded likes array on message JSON; positional indices as IDs.

---

## D006 — Transport field distinct from delivery

**Date:** 2026-06-27  
**Decision:** Add `transport` (local / relay / direct) separate from `delivery` (pending / relayed / failed). UI in E2E mode shows transport; delivery shows send pipeline state.  
**Rationale:** A message can be `delivery=relayed` via `transport=direct` or `transport=relay`; users in private mode need to know fallback path.  
**Alternatives:** Overload `delivery` enum; infer transport in UI from thread type only.

---

## D007 — Converge on ThreadMessage, deprecate dual Conversation path

**Date:** 2026-06-27  
**Decision:** Target state uses `IThreadStore` / `ThreadMessage` as the only durable transcript for AI and P2P. In-memory `Conversation` becomes an implementation detail or is removed from hot paths when messaging is enabled.  
**Rationale:** [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md) already describes conversation-first design; dual models caused persistence gaps for AI home.  
**Alternatives:** Persist `Conversation` separately and sync to threads; SQLite-only unified table without JSON migration path.

---

## Open decisions (not yet resolved)

| ID | Question | Options |
|----|----------|---------|
| O001 | Sidebar UX for two channels per contact | Two session rows; one row + mode toggle; nested under contact |
| O002 | Default for “clear history” re memory | Keep memory (default) vs always wipe memory |
| O003 | Summary storage shape | Top-level key in `{thread_id}.json` vs `{thread_id}.memory.json` sidecar |
| O004 | Annotation sync v1 | Local-only annotations vs relay envelope v2 |

When resolved, move rows to numbered decisions above.
