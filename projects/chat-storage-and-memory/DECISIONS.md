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

## D008 — Sender-assigned `sender_seq` alongside UUID

**Date:** 2026-06-27  
**Decision:** Peer-visible direct `content` messages carry a monotonic **`sender_seq` (uint64) per sender per chat target**, in addition to UUID `message_id`. Seq is **per-sender**, not one global thread counter — in 1:1 chat each participant has an independent stream. UUID remains the sole message identity for dedup and annotations; seq is metadata for ordering and gap detection.  
**Rationale:** UUID dedup prevents duplicates but cannot detect missing messages; gap detection is critical for private chat reliability. Per-sender seq scales to group chat later.  
**Alternatives:** Relay-assigned global log index; positional indices as IDs (rejected in D005); timestamp-only ordering.

---

## D009 — Three sync modes (tail, gap repair, scroll backfill)

**Date:** 2026-06-27  
**Decision:** P2P history sync uses three distinct modes: **(1) tail sync** — on open, reconnect, or new device, fetch latest N (default 50) messages per peer; **(2) gap repair** — automatic backfill when a hole appears in the contiguous tail (have seq N, receive N+2+), not gated on scroll; **(3) history backfill** — when user scrolls to the top of loaded transcript, page older messages (25 per page) via `sender_seq < loaded_min_seq`. Bootstrap/tail ingest on an empty store does **not** treat a high incoming seq as a gap.  
**Rationale:** Lazy history saves bandwidth; live gap repair keeps private chat trustworthy; conflating the two causes false alarms or missed holes.  
**Alternatives:** Full history sync on every open; poll cursor only (no per-thread gap detection).

---

## D010 — Seq lifecycle on chat target

**Date:** 2026-06-27  
**Decision:** `next_outgoing_seq` and `session_epoch` are keyed to **chat target `(contact_id, channel)`**, surviving thread delete/recreate and **clear visible history** (seq is not reset on clear). On clear, receiver sets `history_floor_seq[peer]` and does not auto-backfill older seq unless the user scrolls up. Failed sends retry with the **same `message_id` and `sender_seq`**. New device bootstrap uses tail sync only (same as open/reconnect). `uint64` overflow is accepted as out of scope.  
**Rationale:** Seq represents the long-lived conversation with a contact, not a local transcript snapshot; idempotent retries must not bump seq; clear history is a local display choice, not a protocol reset.  
**Alternatives:** Reset seq on clear; assign seq only after successful relay; thread-scoped counters.

---

## D011 — Session compromise on conflicting seq

**Date:** 2026-06-27  
**Decision:** If the same `(sender, session_epoch, sender_seq)` is received with a **different `message_id`**, treat as **session integrity failure**: halt ingest, notify both parties, rotate E2E keys, bump `session_epoch`, and start a new secure conversation (seq resets **only** for the new epoch). Same `(message_id, sender_seq)` duplicates are benign (UUID dedup). Envelope signature must bind `sender_seq` and `session_epoch`.  
**Rationale:** Under encryption, conflicting seq implies replay, split-brain, or attack; silent merge would break trust in private chat.  
**Alternatives:** Last-write-wins; ignore conflict; log only.

---

## D012 — Three `@ai` modes in direct threads

**Date:** 2026-06-27  
**Decision:** Direct-thread `@ai` has three explicit modes: **(1) Local** — `@ai …`, private assist, not relayed, no sync seq; **(2) Shared reply** — `@ai+ …` (alias `@ai share …`), relay AI output only, +1 sync seq on trigger user’s stream; **(3) Shared full** — `@ai++ …` (alias `@ai share all …`), relay stripped prompt then AI reply, +2 sync seq. Default is local. Shared rows use **`generation=ai_on_behalf`** for the AI reply; wire identity and **`sender_seq`** belong to the **trigger user** (`seq_owner_contact_id=local:self`, envelope `sender_contact_id=local:self`) so peer gap detection stays on the user’s stream. Local `@ai` must not consume sync seq (avoids false gaps when private assists sit between relayed messages).  
**Rationale:** Users need private in-thread AI help without exposing it; when they choose to share, AI “speaks on behalf of” the triggerer with contiguous seq on the wire; prompt-only vs prompt+reply are distinct privacy/product choices.  
**Alternatives:** Single `@ai` always local; always relay AI replies; one seq stream including local assists (rejected — false peer gaps).

---

## Open decisions (not yet resolved)

| ID | Question | Options |
|----|----------|---------|
| O001 | Sidebar UX for two channels per contact | Two session rows; one row + mode toggle; nested under contact |
| O002 | Default for “clear history” re memory | Keep memory (default) vs always wipe memory |
| O003 | Summary storage shape | Top-level key in `{thread_id}.json` vs `{thread_id}.memory.json` sidecar |
| O004 | Annotation sync v1 | Local-only annotations vs relay envelope v2 |
| O005 | Relay tail/gap API shape | Per-thread `fetch_since_seq` vs inbox-wide index; depends on relay server work |

When resolved, move rows to numbered decisions above.
