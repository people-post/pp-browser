# Design — desired end state

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Channel isolation** — public (relay) and E2E conversations with the same contact are different threads with different memory boundaries.
5. **Stable IDs everywhere** — messages, threads, and annotation targets use UUIDs for dedup, sync, and reactions.
6. **Storage abstraction** — `IThreadStore` stays the seam; JSON default now, SQLite optional later.

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

**Thread identity for direct:** `(contact_id, channel)` — never one thread for both modes.

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
| `relay_visible` | bool | `false` for `@ai` assist |
| `delivery` | enum | Outbound lifecycle: local, pending, relayed, failed |
| `transport` | enum | **New.** `local`, `relay`, `direct` — how message arrived/sent |
| `target_message_id` | optional UUID | **New.** For annotations (like, edit, receipt) |
| `annotation_type` | optional string | **New.** e.g. `like`, `edit`, `read_receipt` |

**LLM context** includes only `kind == content` (plus selected `system`), never raw annotations.

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
| **Clear visible history** | delete messages, keep thread shell | rebuilt empty | optional keep (default: keep) | local only |
| **Forget what AI learned** | unchanged | n/a | delete summary/facts | n/a |
| **Start fresh** | new thread | empty | empty | n/a |
| **Delete conversation** | delete thread file + index entry | gone | gone | peer may retain |

P2P clear actions must disclose that remote copies may persist.

## Routing and modes

| Thread kind | User message | Path |
|-------------|--------------|------|
| AI | any (non-payload) | `AgentSession::SubmitToThread` → store → LLM |
| Direct | normal text | `P2pMessagingService::SendUserMessage` → relay (or direct transport) |
| Direct | `@ai …` | `SubmitScopedAssist` — local AI, `relay_visible=false` |
| Direct | structured payload | local action chips — no relay |

## Transport provenance (private / E2E UI)

In E2E threads, show per-message indicator:

- **Direct** — libp2p (or future direct path)
- **Relay** — fell back to relay (privacy-relevant)
- **Local** — `@ai` assist, system, unsent draft

Set `transport` at send/receive in `P2pMessagingService` (and future libp2p layer), not inferred in UI.

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
- **Settings / thread menu** — Clear history, Forget AI memory, Delete conversation (with confirmations).

## Non-goals (for now)

- Cross-device sync protocol design
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
