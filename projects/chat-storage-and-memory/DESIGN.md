# Design — desired end state

## Principles

1. **One transcript model** — UI, disk, and LLM context derive from the same message store (`ThreadMessage` / future extensions), not parallel in-memory shapes.
2. **Local source of truth** — write locally before send; relay rejections do not erase history ([P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md)).
3. **Three storage layers** — distinguish what the user sees, what fits in the LLM window, and what the agent remembers long-term.
4. **Channel isolation** — public (relay) and E2E conversations with the same contact are different threads with different memory boundaries.
5. **Stable IDs everywhere** — messages, threads, and annotation targets use UUIDs for dedup, sync, and reactions.
6. **Sender sequence for completeness** — peer-visible direct messages carry a per-sender monotonic `sender_seq` (in addition to UUID) so receivers detect gaps in the live tail; UUID remains the only message identity. Only **`relay_visible`** content consumes sync seq (see `@ai` modes below).
7. **Strict normal-or-compromised ingest (direct chat)** — in **all direct threads** (`public_relay` and `e2e`), the receiver accepts only messages that match a small set of **normal** cases (D013). Everything else → `sync_state=compromised` (halt ingest, notify; E2E → key rotation / new epoch; public → re-sync UX without PSK rotation). The sender has an explicit **within-epoch contract**; violations are not silently merged.
8. **Durable outbox** — `relay_visible` rows with `delivery=pending` or `failed` survive app restart; retries reuse the same `(message_id, sender_seq)` (D017).
9. **Storage abstraction** — `IThreadStore` stays the seam; JSON default now, SQLite optional later.

## Assumptions (v1)

| Assumption | Implication |
|------------|-------------|
| **Single active sender per identity** (D015) | One client per profile may send on a chat target at a time. Two devices with the same identity and PSK without coordination will emit conflicting `sender_seq` → compromised (D011). Document in UX; multi-device seq coordination is out of scope for v1. |
| **No legacy thread migration** (D016) | Pre-v6 on-disk threads are not upgraded. Schema bumps may require wiping `{data_dir}/profiles/{id}/threads/` (acceptable — no production users yet). |

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
| `session_epoch` | uint32 | **New.** Bumped on E2E key rotation, compromise recovery, device reset, or “new secure chat”; scopes `sender_seq` streams |

**Thread identity for direct:** `(contact_id, channel)` — never one thread for both modes.

### Chat target (long-lived, direct P2P)

Outbound sequence counters and session epochs are keyed to **chat target** `(contact_id, channel)`, not `thread_id`. A thread file may be deleted and recreated for the same person; seq continues until `session_epoch` rotates.

| Field | Scope | Notes |
|-------|-------|-------|
| `next_outgoing_seq` | chat target | Monotonic uint64 per epoch; assigned at first local persist before send |
| `session_epoch` | chat target / thread | Increment on compromise recovery, full device reset, or explicit new secure chat; **only** way to restart seq from 1 (D014) |

Persist in thread metadata or a small sidecar keyed by `(contact_id, channel)`.

### Per-peer sync state (per thread, scoped by `session_epoch`)

Sync watermarks are keyed by **`(peer, session_epoch)`**. A new epoch starts a fresh stream; old-epoch state is retained for history display but not used for gap logic on the new epoch.

| Field | Notes |
|-------|-------|
| `contiguous_peer_seq[peer][epoch]` | Highest seq received with no holes in the active tail for this epoch |
| `loaded_min_seq[peer][epoch]`, `loaded_max_seq[peer][epoch]` | Oldest/newest peer seq present in local transcript for this epoch |
| `history_floor_seq[peer][epoch]` | Set on **clear visible history** — relay-visible seq at or below this in the same epoch is a **protocol violation** (D013) |
| `sync_state` | `ok` \| `gap` \| `compromised` |

### ThreadMessage

| Field | Type | Notes |
|-------|------|-------|
| `id` | UUID | Client-generated; dedup on ingest |
| `thread_id` | UUID | |
| `content_type` | enum | **New.** `text`, `annotation`, `contact_card`, `crypto_tx`, `system` — see § ChatPayload |
| `payload` | JSON object | Type-specific structured body (wire + disk) |
| `text` | optional string | Display snippet / search / plain fallback; AI raw for `text` turns |
| `content_rml` | optional string | Rendered blocks (AI assistant) |
| `user_payload` | optional string | LLM-only structured JSON (AI turns) |
| `chat_actions` | array | Indexed chips |
| `target_message_id` | optional UUID | For `annotation` (and edits referencing prior message) |
| `sender_contact_id` | string | `local:self`, `ai:assistant`, or contact id |
| `timestamp` | int64 | |
| `relay_visible` | bool | `true` when sent to peer; see `@ai` modes |
| `delivery` | enum | local, pending, relayed, failed |
| `transport` | enum | local, relay, direct |
| `control_type` | optional string | For `content_type=system`, e.g. `epoch_start` (D014) |
| `sender_seq` | optional uint64 | Sync seq on `relay_visible` content only |
| `session_epoch` | optional uint32 | Must match envelope |
| `generation` | optional enum | `user` \| `ai_on_behalf` |
| `seq_owner_contact_id` | optional string | Trigger user for `ai_on_behalf` rows |
| `ai_invoke_mode` | optional enum | `local` \| `shared_reply` \| `shared_full` |

**Sync seq rule:** `sender_seq` assigned only when `relay_visible=true`. Local-only rows never consume sync seq.

**Not sequenced:** local `@ai`, local-only annotations (`relay_visible=false`), local-only system rows.

**LLM context:** `content_type=text` plus selected `system` rows only — not annotations, cards, or txs unless summarized in `text`.

### ChatPayload (unified message body — D026)

One schema for disk, relay plaintext (`public_relay`), and AEAD plaintext (`e2e` — E010). Envelope `body.content` holds this object; E2E encrypts its UTF-8 JSON serialization.

```json
{
  "schema_version": 1,
  "content_type": "text",
  "text": "optional human-readable snippet",
  "payload": {}
}
```

| `content_type` | `payload` shape (required keys) | UI |
|----------------|----------------------------------|-----|
| `text` | `{}` or `{"format":"plain"}` | Normal bubble; `text` required |
| `annotation` | `annotation_type`, `target_message_id`; optional `value` (emoji, edit body, etc.) | Inline on target or thread row; badge |
| `contact_card` | `contact_id`, `display_name`; optional `relay_user_id`, `avatar_url` | Contact card chrome; share/add actions |
| `crypto_tx` | `chain_id`, `asset`, `amount`, `direction` (`send`\|`request`); optional `tx_hash`, `status`, `to_address` | Transaction card; explorer link when hash present |
| `system` | `control_type` (e.g. `epoch_start`); optional `detail` | Centered system line |

**Annotation example** (relayed like — consumes seq when `relay_visible=true`):

```json
{
  "schema_version": 1,
  "content_type": "annotation",
  "text": "👍",
  "payload": {
    "annotation_type": "reaction",
    "target_message_id": "uuid-of-target",
    "value": { "emoji": "👍" }
  }
}
```

**Contact card example:**

```json
{
  "schema_version": 1,
  "content_type": "contact_card",
  "text": "Alice",
  "payload": {
    "contact_id": "contact:abc",
    "display_name": "Alice",
    "relay_user_id": "relay:user:xyz"
  }
}
```

**Crypto transaction example:**

```json
{
  "schema_version": 1,
  "content_type": "crypto_tx",
  "text": "Sent 0.5 ETH",
  "payload": {
    "chain_id": "eip155:1",
    "asset": "ETH",
    "amount": "0.5",
    "direction": "send",
    "tx_hash": "0x…",
    "status": "confirmed"
  }
}
```

`BuildDisplayRows` merges `annotation` rows onto `target_message_id` for rendering; other types render as standalone rows with type-specific templates.

### Durable memory (per thread)

| Artifact | Location | Notes |
|----------|----------|-------|
| `ConversationSummary` | `threads/{thread_id}/memory.json` | Text + version; from `ICompactionService` |
| Future fact rows | Same file | Deferred |

## On-disk layout (target — D025)

Profile-scoped JSON default:

```
{data_dir}/profiles/{profile_id}/
  threads/
    index.json                      # sidebar metadata only (id, kind, channel, title, preview, …)
    {thread_id}/
      messages.json                 # { schema_version, messages: [...] }
      memory.json                   # ConversationSummary + future facts
      sync.json                     # per-thread sync watermarks (v6)
  sync/
    chat_targets.json               # next_outgoing_seq, session_epoch per (contact_id, channel)
```

Delete thread = remove `{thread_id}/` directory + index entry. Optional v5: `threads.db` replaces directory tree via `IThreadStore`.

### Schema versioning (breaking)

- Thread and message JSON carry `schema_version`. **No in-place migration** from pre-v6 layouts (D016).
- When v2b/v6 land, bump version and document “delete profile threads dir or reinstall” for dev builds.
- `JsonThreadStore` writes atomically: temp file + rename (v2a) to avoid crash-corrupt transcripts.

## Clear / forget semantics (user-facing — D024)

**Clear history** opens a **choice sheet** with three levels (labels illustrative):

| Level | Transcript | LLM window | `memory.json` | Thread shell | P2P `history_floor_seq` |
|-------|------------|------------|---------------|--------------|-------------------------|
| **Clear messages** | wipe | empty | keep | keep | set per peer/epoch |
| **Clear messages & AI memory** | wipe | empty | wipe | keep | set per peer/epoch |
| **Delete conversation** | gone | gone | gone | delete index + dir | n/a |

**Forget what AI learned** (separate menu item): transcript unchanged; wipe `memory.json` only.

| Other action | Transcript | Memory | Notes |
|--------------|------------|--------|-------|
| **New chat** (AI) | new empty thread dir | empty | n/a |

P2P clear levels include copy that peer and relay may retain copies. Clear visible levels do **not** reset outgoing seq or `session_epoch` (D010).

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
  "sender_contact_id": "contact:…",
  "sender_seq": 42,
  "session_epoch": 1,
  "body": {
    "content": {
      "schema_version": 1,
      "content_type": "text",
      "text": "Hello",
      "payload": {}
    }
  },
  "timestamp": 1234567890,
  "signature": "…"
}
```

**E2E channel** — same outer envelope; `body` uses nested ciphertext (E009):

```json
"body": {
  "e2e": {
    "payload_b64": "…"
  }
}
```

`payload_b64` decodes to `[version:1][nonce:24][ciphertext+tag]`; AEAD plaintext is UTF-8 JSON of the same `ChatPayload` object (E010).

- **`sender_contact_id`** is required on the wire (D021). Do not infer sender from thread participants (breaks group chat and multi-peer correctness).
- Signature covers `(message_id, sender_seq, session_epoch, thread_id, sender_contact_id, …)` so seq cannot be forged independently of the sender key.

### Send pipeline

1. Serialize `next_outgoing_seq` assignment per chat target (mutex or store-level increment).
2. Assign `(message_id, sender_seq)` at first local persist (`relay_visible=true` only).
3. Persist with `delivery=pending` **before** network I/O.
4. Sign and send; on failure set `delivery=failed` and enqueue durable retry (D017).
5. Retries reuse the **same** `(message_id, sender_seq)` — only `delivery` and transport may change.

### Durable outbox (D017)

| Source | Behavior |
|--------|----------|
| On startup | Scan all threads for `relay_visible` rows with `delivery ∈ {pending, failed}`; re-enqueue send |
| In-memory queue | May batch IO; must not be the sole copy of pending state |
| Retry policy | Exponential backoff; cap attempts per message; user-visible notice on persistent failure |
| Relay dedup | Server must accept duplicate `message_id` on retry (idempotent ingest) |

## P2P sync (direct / E2E)

Three **separate** sync modes — do not conflate lazy history with live gap repair:

| Mode | Trigger | Behavior |
|------|---------|----------|
| **Tail sync** | Open thread, reconnect, new device | Fetch latest **N** peer-visible messages per sender (default **50**) |
| **Gap repair** | Hole in contiguous tail (`seq N` + `seq N+2+`) | Automatic backfill from peer (direct) or relay fallback; **not** gated on scroll |
| **History backfill** | User scrolls to top of loaded transcript | Page older messages (`sender_seq < loaded_min_seq`); page size **25** |

### Within-epoch sender contract

For a fixed chat target `(contact_id, channel, session_epoch)`, the **sender** must obey:

| Rule | Behavior |
|------|----------|
| S1 | Assign `sender_seq` only when `relay_visible=true`; strictly monotonic 1, 2, 3, … within the epoch |
| S2 | `next_outgoing_seq` never decreases within an epoch |
| S3 | **Clear visible history** (local UI) does **not** reset seq |
| S4 | Failed send retries the **same** `(message_id, sender_seq)` |
| S5 | Local-only rows (`@ai`, annotations, local system) do **not** consume seq |
| S6 | The **only** way to emit `sender_seq = 1` again is a **new `session_epoch`** (D014) |
| S7 | Never emit relay-visible content with `sender_seq < next_outgoing_seq` (no reuse, no rewind) |

Receiver treats sender violations as compromised ingest (D013), not best-effort merge.

### Bootstrap vs gap

- **Bootstrap / tail ingest:** empty per-epoch transcript (or new `session_epoch`) may receive high `sender_seq` without backfilling all prior seq — not a gap alarm (D009).
- **New epoch:** `session_epoch` increases → reset per-epoch watermarks for that peer; `sender_seq = 1` is normal bootstrap, not compromised.
- **Contiguous gap:** local state for this epoch already has seq **N** and receives **N+2+** above `history_floor_seq` → `sync_state=gap`, attempt repair (not yet compromised).

E2E tail sync is **peer-first** (direct/libp2p); relay tail is fallback when the peer is offline or transport fell back.

### Relay API — thread message fetch (D027)

Authoritative backfill when peer is offline or for gap/history sync. Authenticated as relay user (same identity as send).

**`GET /v1/threads/{thread_id}/messages`**

| Query param | Required | Description |
|-------------|----------|-------------|
| `sender_contact_id` | yes | Peer's contact id whose seq stream to fetch |
| `session_epoch` | yes | Epoch scope |
| `min_sender_seq` | no | Inclusive lower bound (gap repair) |
| `max_sender_seq` | no | Inclusive upper bound (history backfill) |
| `limit` | no | Default **50**, max **100** |
| `order` | no | `asc` (default) or `desc` |

**Sync mode usage:**

| Mode | Typical request |
|------|-----------------|
| Tail sync | `order=desc`, `limit=50`, no min/max |
| Gap repair | `min_sender_seq=N+1`, `max_sender_seq=M`, `order=asc` |
| History backfill | `max_sender_seq=loaded_min-1`, `limit=25`, `order=desc` |

**Response 200:**

```json
{
  "thread_id": "uuid",
  "session_epoch": 1,
  "sender_contact_id": "contact:…",
  "messages": [ /* RelayEnvelope[] */ ],
  "has_more": true,
  "cursor": {
    "next_min_sender_seq": 10,
    "next_max_sender_seq": null
  }
}
```

- Each element is a full signed `RelayEnvelope` (client verifies signature, runs D013 ingest).
- **`POST /v1/messages`** (or existing send): idempotent on `message_id` — duplicate POST returns 200 with same id (D017).
- Inbox **poll** may remain for notifications; clients must not rely on poll alone for seq-complete history.

MCP bridge: expose equivalent `relay_fetch_thread_messages` tool with same parameters for promoted-MCP path.

### Reorder buffer (D020)

During gap repair or multi-path delivery (direct + relay), messages may arrive out of order.

- **`kReorderWindow = 32`** — hold inbound messages with `sender_seq` in `(contiguous_peer_seq, contiguous_peer_seq + kReorderWindow]` before declaring `gap`.
- Messages above the window without filling the hole → `sync_state=gap`, trigger repair.
- After repair, flush buffer in seq order before updating `contiguous_peer_seq`.

### Display ordering (D019)

UI transcript sort (stable):

1. **`relay_visible` content:** `(session_epoch, sender_contact_id, sender_seq)` ascending.
2. **Local-only rows** (`relay_visible=false`): interleaved by `timestamp` among themselves; never sorted between another sender’s sequenced rows.
3. **Tie-break:** `timestamp`, then `message_id`.

Gap repair may reorder visually when missing rows arrive; scroll position should anchor on read cursor, not raw array index.

### Receive pipeline (direct / E2E)

Ordered steps — do not reorder in implementation:

1. **UUID dedup** — benign duplicate → stop.
2. **Verify Ed25519 signature** on outer envelope (classical; see e2e-message-crypto).
3. **Thread / channel / epoch** — reject wrong `thread_id` or epoch mismatch before decrypt.
4. **Decrypt AEAD** (`e2e` only) — canonical AAD must match envelope fields; failure → reject (no persist).
5. **D013 ingest classifier** — normal · gap · compromised; reorder buffer before gap declaration.
6. **Persist** — append `ThreadMessage`, update watermarks, set `transport` from ingress path.

### Ingest classification (normal · gap · compromised)

After steps 1–3 above (signature verified; decrypt if E2E):

**Normal (accept):**

1. **Benign duplicate** — same `(message_id, sender_seq, session_epoch)` → ignore.
2. **Epoch advance** — `session_epoch` increases → reset per-epoch watermarks; accept as fresh stream (see § Peer reset).
3. **Contiguous tail** — `sender_seq == contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]`.
4. **Tail bootstrap** — per-epoch transcript empty; ingest tail batch without requiring seq 1..N first.
5. **Authorized backfill** — `sender_seq` in `(history_floor_seq, loaded_min_seq)` only when user/system initiated history backfill for that range.

**Gap (repair allowed; not compromised until repair fails):**

- `sender_seq > contiguous_peer_seq + 1` and `sender_seq > history_floor_seq[peer][epoch]` → request missing range; on success, reclassify as normal.
- If repair returns floor violations, seq conflicts, or impossible ranges → **compromised**.

**Compromised (halt ingest immediately):**

| Condition | Why |
|-----------|-----|
| `session_epoch` **decreases** | Illegal rollback |
| Same `(peer, epoch, sender_seq)` + **different** `message_id` | Seq conflict (D011) |
| `sender_seq ≤ history_floor_seq[peer][epoch]` | Replay, stale traffic, or sender reset without epoch bump |
| `sender_seq < contiguous_peer_seq` and not benign duplicate | Rewind within epoch |
| `sender_seq = 1` in an **established** epoch where `contiguous_peer_seq > 0` | Sender reset without epoch bump |
| Invalid signature / wrong thread / envelope epoch mismatch | Wire invalid |
| Gap repair exhausted or returns violating messages | Repair failed |

On compromised: halt ingest, block further sends on the affected chat target, notify user. Recovery: see § Compromise recovery.

**Channel-specific recovery:**

| Channel | Compromised UX |
|---------|----------------|
| `e2e` | Halt ingest; manual new PSK + `session_epoch` bump (D011, D014); optional `epoch_start` system row |
| `public_relay` | Halt ingest; “conversation integrity problem” — user may delete thread and start fresh or contact support; no PSK rotation |

### Compromise recovery (`e2e`)

Local state machine per chat target: `ok` → `gap` → `compromised` → `awaiting_new_psk` → `ok`.

```
Compromised side                          Innocent peer
     |                                         |
     | 1. Halt ingest + outbound on target     | 1. Halt ingest on target
     | 2. Show "Start new secure chat"         | 2. Banner: peer session reset
     | 3. User exchanges new PSK OOB           | 3. Accept higher session_epoch
     | 4. session_epoch++ locally              |    when epoch_start or first
     | 5. Send epoch_start (seq=1)             |    sequenced message arrives
     | 6. Resume at seq=2+                     | 4. Fresh watermarks for new epoch
```

- **Who bumps first:** compromised side initiates after user confirms; innocent peer accepts **strictly higher** `session_epoch` as bootstrap (D014).
- **Old epoch keys:** retain locally for decrypting historical ciphertext only; do not ingest new traffic on old epoch after compromise.
- **Both sides compromised:** each user runs the flow independently; coordinated PSK exchange is manual (E001).

### Clear history and seq

| Party | Behavior |
|-------|----------|
| **Sender** | `next_outgoing_seq` and `session_epoch` on chat target unchanged; next live send uses next seq as usual (e.g. 101) |
| **Receiver** | Wipe messages; set `history_floor_seq[peer][epoch]` to max contiguous seq seen at clear time; do not auto-backfill seq at or below floor unless user scrolls up |
| **Live traffic after clear** | Accept peer messages with `sender_seq > floor`; any `sender_seq ≤ floor` in same epoch → **compromised** (D013) |

The sender does not need a signal that the peer cleared locally; honest senders continue forward. Replays and protocol violators are caught by the floor rule.

### Peer reset / new device (fresh stream)

When a peer wipes local state, installs on a new device without backup, or explicitly starts over:

1. **Bump `session_epoch`** on the chat target (mandatory — D014).
2. Reset `next_outgoing_seq = 1` for the new epoch only.
3. Optionally send `content_type=system`, `control_type=epoch_start` as the first relay-visible row (`sender_seq=1`); user content continues at seq 2+.
4. **Receiver** on unseen higher epoch: fresh per-epoch watermarks; `sender_seq=1` is normal bootstrap.

**Restored backup** (same identity + chat-target sidecar): not a reset — continue same epoch and seq.

Sending `sender_seq=1` without bumping epoch in an established epoch is always **compromised**.

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

- **Sidebar groups (D023)** — collapsible sections: **AI**, **Public** (`public_relay` direct), **Private** (`e2e` direct). Same contact may appear in Public and Private.
- **E2E vs public shell** — `.chat-shell--e2e` / `.chat-shell--public` ([UI_DESIGN_SYSTEM.md](../../docs/UI_DESIGN_SYSTEM.md)).
- **Message row** — transport badge (E2E); delivery state; type-specific templates for `contact_card`, `crypto_tx`, `annotation`.
- **Gap banner** — when `sync_state=gap`; tap to retry sync.
- **Scroll hint** — when `loaded_min_seq > 1`.
- **`@ai` modes** — composer hints; confirm for `@ai+` / `@ai++`.
- **Clear history (D024)** — choice sheet: clear messages / clear messages & memory / delete conversation.
- **Forget AI memory** — separate action (memory sidecar only).

## Non-goals (for now)

- Full cross-device history mirror (tail + scroll backfill only; see P2P sync above)
- **Multi-device concurrent send** on one identity (D015 — single active device v1)
- **Legacy on-disk migration** from pre-v6 thread JSON (D016)
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
- [ ] Direct threads assign `sender_seq` on send; receiver detects tail gaps and auto-repairs (direct + relay D027).
- [ ] Sidebar shows AI / Public / Private groups (D023).
- [ ] Clear history choice sheet implements D024 levels.
- [ ] `ChatPayload` types render: text, annotation, contact_card, crypto_tx (D026).
- [ ] Pending `relay_visible` messages survive restart and retry with same `(message_id, sender_seq)` (D017).
- [ ] Clear history preserves seq counters and epoch; `sender_seq ≤ history_floor` in same epoch triggers compromised UX.
- [ ] Peer reset bumps `session_epoch`; `sender_seq=1` on new epoch accepted; same-epoch rewind triggers compromised UX.
- [ ] Duplicate `(sender, session_epoch, sender_seq)` with conflicting `message_id` triggers key rotation UX (`e2e`) or integrity UX (`public_relay`).
- [ ] `@ai` local vs `@ai+` / `@ai++` shared modes behave per routing table; shared rows use trigger user’s sync seq.
- [ ] Transcript display order follows D019; reorder buffer (D020) prevents false gaps during benign reorder.
