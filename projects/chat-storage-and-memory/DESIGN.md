# Design — desired end state

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Channel isolation** — public (relay) and E2E conversations with the same contact are different threads with different memory boundaries.
5. **Stable IDs everywhere** — messages, threads, and annotation targets use UUIDs for dedup, sync, and reactions.
6. **Sender sequence for completeness** — peer-visible direct messages carry a per-sender monotonic `sender_seq` (in addition to UUID) so receivers detect gaps in the live tail; UUID remains the only message identity. Only **`relay_visible`** content consumes sync seq (see `@ai` modes below).
7. **Storage abstraction** — `IThreadStore` stays the seam; JSON default now, SQLite optional later.

## Three layers (transcript vs context vs memory)

```
┌─────────────────────────────────────────────────────────────┐
│ Layer 1: UI transcript                                      │
│   ThreadMessage[] on disk — full history user can scroll    │
├─────────────────────────────────────────────────────────────┤
│ Layer 2: LLM context window                                 │
│   IContextPolicy / ThreadContextPolicy — sliding trim       │
│   rebuilt each turn; not a user-facing “store”              │
├─────────────────────────────────────────────────────────────┤
│ Layer 3: Durable agent memory                               │
│   ConversationSummary + optional fact store per thread      │
│   survives trimming; cleared independently of transcript    │
└─────────────────────────────────────────────────────────────┘
```

Tool-call scratch (`turn_scratch`) remains **ephemeral per turn only** — never persisted as chat bubbles.

## Data model (target)

### Thread

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Stable; used in relay envelope |
| `kind` | `ai` \| `direct` \| `group` | Unchanged |
| `channel` | `public_relay` \| `e2e` | **New.** Replaces overloading `encrypted` alone |
| `participant_contact_ids` | string[] | One peer for direct |
| `title`, `preview`, `updated_at`, `unread_count` | — | Sidebar metadata |
| `encrypted` | bool | Derived from `channel == e2e` (keep for UI binding) |
| `session_epoch` | uint32 | **New.** Bumped on E2E key rotation / “new secure chat”; scopes `sender_seq` streams |

**Thread identity for direct:** `(contact_id, channel)` — never one thread for both modes.

### Chat target (long-lived, direct P2P)

Outbound sequence counters and session epochs are keyed to **chat target** `(contact_id, channel)`, not `thread_id`. A thread file may be deleted and recreated for the same person; seq continues until `session_epoch` rotates.

| Field | Scope | Notes |
|-------|-------|-------|
| `next_outgoing_seq` | chat target | Monotonic uint64; assigned at first local persist before send |
| `session_epoch` | chat target / thread | Increment on compromise recovery or explicit new secure chat |

Persist in thread metadata or a small sidecar keyed by `(contact_id, channel)`.

### Per-peer sync state (per thread)

| Field | Notes |
|-------|-------|
| `contiguous_peer_seq[peer]` | Highest seq received with no holes in the active tail |
| `loaded_min_seq[peer]`, `loaded_max_seq[peer]` | Oldest/newest peer seq present in local transcript |
| `history_floor_seq[peer]` | Set on **clear visible history** — seq at or below this are intentionally not loaded |
| `sync_state` | `ok` \| `gap` \| `compromised` |

### ThreadMessage

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Client-generated; dedup on ingest |
| `thread_id` | UUID | |
| `kind` | `content` \| `annotation` \| `system` | **New.** Default `content` |
| `sender_contact_id` | string | `local:self`, `ai:assistant`, or contact id |
| `text` | string | Plain or LLM raw for AI turns |
| `content_rml` | optional string | Rendered blocks |
| `user_payload` | optional string | LLM-only structured JSON (AI turns) |
| `chat_actions` | array | Indexed chips |
| `timestamp` | int64 | |
| `relay_visible` | bool | `true` only when message is sent to peer; see `@ai` modes |
| `delivery` | enum | Outbound lifecycle: local, pending, relayed, failed |
| `transport` | enum | **New.** `local`, `relay`, `direct` — how message arrived/sent |
| `target_message_id` | optional UUID | **New.** For annotations (like, edit, receipt) |
| `annotation_type` | optional string | **New.** e.g. `like`, `edit`, `read_receipt` |
| `sender_seq` | optional uint64 | **New.** Sync seq on `relay_visible` content only; per-sender per chat target |
| `session_epoch` | optional uint32 | **New.** Must match envelope; scopes seq after key rotation |
| `generation` | optional enum | **New.** `user` \| `ai_on_behalf` — who produced the text; shared AI rows are `ai_on_behalf` |
| `seq_owner_contact_id` | optional string | **New.** Trigger user for `ai_on_behalf` rows (`local:self`) |
| `ai_invoke_mode` | optional enum | **New.** Audit: `local` \| `shared_reply` \| `shared_full` on assist-related rows |

**Sync seq rule:** `sender_seq` is assigned and incremented only when `relay_visible=true`. Local-only rows never consume sync seq — avoids false peer gap detection when the user had private `@ai` assists between relayed messages.

**Not sequenced (no sync seq):** local `@ai`, annotations, and local system rows.

**LLM context** includes only `kind == content` (plus selected `system`), never raw annotations. Shared `@ai` rows in the transcript participate like normal user/assistant content.

### Durable memory (per thread)

| Artifact | Location (initial) | Notes |
|----------|-------------------|-------|
| `ConversationSummary` | `{thread_id}.json` top-level or `{thread_id}.memory.json` | Text + version; from `ICompactionService` |
| Future fact rows | Same file or SQLite | Deferred |

## On-disk layout (target, v2–v3)

Profile-scoped JSON remains default:

```
{data_dir}/profiles/{profile_id}/
  threads/index.json              # thread metadata only
  threads/{thread_id}.json          # messages[] + optional summary
```

Optional v5:

```
  threads.db                        # SQLite: threads, messages, annotations, memory
```

## Clear / forget semantics (user-facing)

| Action | Transcript | LLM window | Durable memory | Peer / relay |
|--------|------------|------------|----------------|--------------|
| **New chat** (AI) | new empty thread | empty | empty | n/a |
| **Clear visible history** | delete messages, keep thread shell; set `history_floor_seq` per peer | rebuilt empty | optional keep (default: keep) | local only; **does not reset** outgoing or peer seq counters |
| **Forget what AI learned** | unchanged | n/a | delete summary/facts | n/a |
| **Start fresh** | new thread | empty | empty | n/a |
| **Delete conversation** | delete thread file + index entry | gone | gone | peer may retain |

P2P clear actions must disclose that remote copies may persist.

## Routing and modes

| Thread kind | User message | Path |
|-------------|--------------|------|
| AI | any (non-payload) | `AgentSession::SubmitToThread` → store → LLM |
| Direct | normal text | `P2pMessagingService::SendUserMessage` → relay (or direct transport) |
| Direct | `@ai …` | Local assist — see table below |
| Direct | structured payload | local action chips — no relay |

### `@ai` in direct threads (three modes)

Composer syntax (local is default for privacy):

| Mode | Syntax | Peer sees | Sync `sender_seq` | Notes |
|------|--------|-----------|-------------------|-------|
| **Local** | `@ai …` | Nothing | No | Private copilot; `relay_visible=false`, `ai_invoke_mode=local` |
| **Shared reply** | `@ai+ …` | AI output only | +1 (reply row) | Prompt not relayed; AI speaks on behalf of trigger user |
| **Shared full** | `@ai++ …` | Prompt + AI output | +2 (prompt, then reply) | Prompt body is stripped text (no `@ai++` prefix on wire) |

Long-form aliases optional in parser: `@ai share …` → shared reply; `@ai share all …` → shared full.

**Shared modes — AI on behalf of trigger user:**

- Trigger user owns **`seq_owner_contact_id`** and the **`sender_seq`** stream on the wire.
- Envelope **`sender_contact_id`** = trigger user (`local:self`) for gap detection on the peer’s view of your stream.
- Local UI may still render `ai_on_behalf` rows as assistant bubbles with “Shared” / “AI assisted” badge.
- **`generation`:** prompt row (shared full only) = `user`; AI reply = `ai_on_behalf`.
- Assign `(message_id, sender_seq)` at first local persist; relay after store; failed send retries same pair (D010).

**Local mode flow:** `SubmitScopedAssist` → persist AI row with `sender_contact_id=ai:assistant`, `relay_visible=false`, no `sender_seq`.

**Shared reply flow:** `SubmitScopedAssist(shared_reply)` → on complete, persist + send one row (`generation=ai_on_behalf`, `relay_visible=true`, +1 seq).

**Shared full flow:** persist + send prompt row (`generation=user`, stripped text, seq N) → `SubmitScopedAssist(shared_full)` → on complete, persist + send reply row (`generation=ai_on_behalf`, seq N+1).

**UX:** confirm before first shared send in a thread (copy differs for `@ai+` vs `@ai++`); E2E transport badge on shared rows. Placeholder: `Message… · @ai · @ai+ · @ai++`.

## Transport provenance (private / E2E UI)

In E2E threads, show per-message indicator:

- **Direct** — libp2p (or future direct path)
- **Relay** — fell back to relay (privacy-relevant)
- **Local** — local `@ai`, system, unsent draft

Set `transport` at send/receive in `P2pMessagingService` (and future libp2p layer), not inferred in UI.

## Relay / direct envelope (target extension)

UUID dedup unchanged. Add fields for ordering, session scope, and signed integrity:

```json
{
  "thread_id": "uuid",
  "message_id": "uuid",
  "sender_relay_id": "relay:…",
  "sender_seq": 42,
  "session_epoch": 1,
  "body": { "text": "…" },
  "timestamp": 1234567890,
  "signature": "…"
}
```

Signature covers `(message_id, sender_seq, session_epoch, thread_id, …)` so seq cannot be forged independently of the sender key.

**Send pipeline:** assign `(message_id, sender_seq)` at first local persist; failed sends retry with the **same** pair — only `delivery` changes.

## P2P sync (direct / E2E)

Three **separate** sync modes — do not conflate lazy history with live gap repair:

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Tail sync** | Open thread, reconnect, new device | Fetch latest **N** peer-visible messages per sender (default **50**) |
| **Gap repair** | Hole in contiguous tail (`seq N` + `seq N+2+`) | Automatic backfill from peer (direct) or relay fallback; **not** gated on scroll |
| **History backfill** | User scrolls to top of loaded transcript | Page older messages (`sender_seq < loaded_min_seq`); page size **25** |

### Bootstrap vs gap

- **Bootstrap / tail ingest:** empty or new-device store may receive high `sender_seq` without backfilling all prior seq — not a gap alarm.
- **Contiguous gap:** only when local state already has seq **N** and receives **N+2+** within the active tail window.

E2E tail sync is **peer-first** (direct/libp2p); relay tail is fallback when the peer is offline or transport fell back.

### Gap repair flow

1. Ingest: dedup by `message_id` (existing).
2. If `(sender, sender_seq, session_epoch)` already seen with a **different** `message_id` → **session compromised** (see below).
3. If `sender_seq == contiguous_peer_seq + 1` → advance watermark.
4. If `sender_seq > contiguous_peer_seq + 1` in tail window → set `sync_state=gap`, request missing range from peer/relay with backoff.

### Clear history and seq

- **Sender:** `next_outgoing_seq` on chat target is unchanged.
- **Receiver:** wipe messages; set `history_floor_seq[peer]`; do not auto-backfill seq below floor unless the user scrolls up or taps “Load earlier messages.”
- Live messages after clear still extend `contiguous_peer_seq` forward normally.

### Session integrity failure

When the same `(sender, session_epoch, sender_seq)` arrives with a **different** `message_id`:

1. Halt ingest for that thread/session.
2. Notify both parties: integrity problem — start a new secure conversation.
3. Rotate keys, bump `session_epoch`, new thread (or epoch-scoped thread); **reset seq counters only for the new epoch**.

Benign duplicate delivery (same `message_id` + same `sender_seq`) is ignored via UUID dedup.

## Store interface (target)

Extend `IThreadStore`:

```cpp
// Illustrative — names may change during implementation
virtual Roe<void> ClearMessages(const std::string& thread_id) = 0;
virtual Roe<void> SetThreadMemory(const std::string& thread_id, ConversationSummary summary) = 0;
virtual Roe<std::optional<ConversationSummary>> GetThreadMemory(const std::string& thread_id) const = 0;
virtual Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel) = 0;
```

Keep `DeleteThread`, `AppendMessage`, `UpdateMessage`, `HasMessageId`.

## UI (target)

- **E2E vs public shell** — existing `.chat-shell--e2e` / `.chat-shell--public` ([UI_DESIGN_SYSTEM.md](../../docs/UI_DESIGN_SYSTEM.md)).
- **Thread list** — one or two entries per contact (TBD in open questions).
- **Message row** — optional transport badge in E2E mode; delivery state for outbound pending/failed.
- **Gap banner** — non-blocking “Possible missing messages” when `sync_state=gap`; tap to retry sync.
- **Scroll hint** — when `loaded_min_seq > 1`, indicate older history available on scroll-up.
- **`@ai` modes** — composer hints; confirm dialog for `@ai+` / `@ai++` before first shared send in E2E/direct threads.
- **Settings / thread menu** — Clear history, Forget AI memory, Delete conversation (with confirmations).

## Non-goals (for now)

- Full cross-device history mirror (tail + scroll backfill only; see P2P sync above)
- Full-text search UI (SQLite enables later)
- Group E2E
- Retraction / “unsend” on relay (future protocol work)

## Success criteria

- [ ] All AI sidebar threads persist across restart via `IThreadStore` (no orphan `Conversation`-only path).
- [ ] User can clear history without deleting thread metadata.
- [ ] User can forget AI memory without losing visible transcript (and vice versa).
- [ ] Same contact can have separate public and E2E threads.
- [ ] Message IDs stable; relay dedup works; annotations reference targets by ID.
- [ ] E2E thread shows relay vs direct per message when transport is known.
- [ ] Direct threads assign `sender_seq` on send; receiver detects tail gaps and auto-repairs.
- [ ] Clear history preserves seq counters; scroll-up loads older pages on demand.
- [ ] Duplicate `(sender, session_epoch, sender_seq)` with conflicting `message_id` triggers key rotation UX.
- [ ] `@ai` local vs `@ai+` / `@ai++` shared modes behave per routing table; shared rows use trigger user’s sync seq.
