# P2P messaging

Person-to-person chat in pp-browser uses a **foundation-first** architecture: one `ThreadMessage` model for AI home, direct, and future group threads; local JSON as source of truth; HTTP relay/directory/registration transport with promoted-MCP and mock fallbacks.

## Service resolution

[`CreateServiceClients`](../src/base/net/ServiceClientFactory.cpp) picks an implementation per endpoint:

1. `base_url` set → HTTP client (`HttpRelayClient`, etc.)
2. else promoted MCP running → MCP bridge (`McpRelayClient`, etc.)
3. else in-process mock (dev default)

Native messaging code (`P2pMessagingService`, `MessagingTools`) always calls `IRelayClient` / `IDirectoryClient` / `IRegistrationClient`; the factory swaps implementations underneath. See [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md) for the MCP infra tool contract.

## Data model

### Thread

| Field | Description |
|-------|-------------|
| `id` | UUID; **local only** — not on wire (D056) |
| `kind` | `ai`, `direct`, `group` |
| `participant_contact_ids` | One peer for direct; N for group (future) |
| `unread_count` | Sidebar badge |
| `preview` | Last message snippet |

### ThreadMessage

| Field | Description |
|-------|-------------|
| `sender_contact_id` | **Local rows:** `local:self`, `ai:assistant`. **Wire / peer rows:** communicating identity value (D079, D082), e.g. `relay:abc123` |
| `content_rml` | Rendered assistant blocks (optional) |
| `relay_visible` | `false` for `@ai` assist (never relayed) |
| `delivery` | `local`, `pending`, `relayed`, `failed` |

Special IDs: `local:self`, `ai:assistant`.

## Persistence

Profile-scoped layout (see [CONFIGURATION.md](CONFIGURATION.md)). **Legacy (today):**

```
{data_dir}/profiles/{profile_id}/
  identity.json
  contacts.json
  threads/index.json
  threads/{thread_id}.json
```

**Target (v2a+):** see [chat-storage-and-memory DESIGN.md](../projects/chat-storage-and-memory/DESIGN.md) — `profile.db` (`threads` + `outbox`) + per-thread `thread.db`; no `index.json`.

Configure endpoints via user config (`~/.config/pp-browser/config.json` on Linux) or in-app **Settings**:

```json
{
  "data_dir": "~/.local/share/pp-browser",
  "relay": { "base_url": "" },
  "directory": { "base_url": "" },
  "registration": { "base_url": "" }
}
```

Empty `base_url` uses promoted MCP infra tools when the promoted MCP client is running; otherwise in-process mocks.

## Relay envelope (target — D056)

**No `thread_id` on the wire.** Full spec: [chat-storage DESIGN § Relay envelope](../projects/chat-storage-and-memory/DESIGN.md#relay--direct-envelope-d056).

```json
{
  "message_id": "uuid",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": { "kind": "direct", "channel": "public_relay" },
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

Inbound routing: `ChatTargetKey { peer_identity_kind, peer_identity_value: sender_contact_id, channel }` → existing local thread when row exists (**E2E inbound find-only**, D062; **public ephemeral** without row, D080). Legacy envelopes with `thread_id` or flat `body.text` (no `body.content`) are rejected.

**Signature verify (E014, E016):** Inbound messages are verified with **`EnvelopeSigner::Verify`** using the sender's **`signing_public_key_b64`** from **`PeerSigningKeyStore`** (directory at add-contact; lazy `GET /v1/users/{relay_user_id}` on cache miss). See [e2e DECISIONS E016](../projects/e2e-message-crypto/DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add).

**Wire cutover (D063):** v2a-p2p ships final envelope + minimal ChatPayload in `body.content`. v4 adds validation only — no second wire break. See [DESIGN § Wire cutover phasing](../projects/chat-storage-and-memory/DESIGN.md#wire-cutover-phasing-d063).

Local store is written **before** send. Server rejections do not delete history. **Unsent/failed** rows stay local — user **retries send**; **peer sync** (`FetchChatTargetMessages`, D058) fetches **missing messages from the peer**, not your pending outbox.

## E2E history sync (v6 — D058–D060)

| Mode | Trigger |
|------|---------|
| Tail sync | Open E2E thread, reconnect |
| Gap repair | Automatic on seq hole |
| User sync | Thread menu **Sync with peer** (D059) |

**Transport:** libp2p peer-direct `/pp-browser/chat-history/1.0.0` first; relay `GET /v1/chat-targets/messages` fallback. Full spec: [chat-storage DESIGN § P2P sync](../projects/chat-storage-and-memory/DESIGN.md#p2p-sync-e2e-only--d045).

Scroll-to-top backfill is **`[post-v1]`** (D052); uses the same fetch primitive.

## AI-centric UX

- **No dedicated search UI** — use agent tools: `search_people`, `list_contacts`, `list_conversations`, `open_conversation`, `start_conversation`.
- Results render as `long_list` blocks with **Message** / **Add contact** chips (`send_chat_action` + JSON `payload`).
- **Registration** via `register_user` and `update_profile_nickname` tools (no sidebar banner).

## @ai in direct threads

Composer: `Message… or @ai ask assistant`

- Pattern: `^@ai\s+(.+)` (case-insensitive)
- **Local only** — not relayed; appended as `ai:assistant` message with `relay_visible=false`
- Uses `AgentSession::SubmitScopedAssist` with thread transcript context

## Modules

| Path | Role |
|------|------|
| `src/feature/messaging/MessagingHub.*` | Wiring, lifecycle |
| `src/feature/messaging/InboxController.*` | Active thread, display rows |
| `src/feature/messaging/P2pMessagingService.*` | Send, poll, dedup |
| `src/feature/messaging/MessageRouter.*` | Composer routing |
| `src/feature/messaging/ContactActionDispatcher.*` | Chip payloads |
| `src/feature/ai/tools/MessagingTools.*` | Agent tool definitions |

## Group chat (future)

`ThreadKind::Group` and `participant_contact_ids[]` are reserved. Adding groups does not require a new message schema.
