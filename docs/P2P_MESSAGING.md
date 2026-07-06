# P2P messaging

Person-to-person chat in pp-browser uses a **foundation-first** architecture: one `ThreadMessage` model for AI home, direct, and future group threads; local persistence as source of truth; HTTP relay/directory/registration transport with mock fallback when `base_url` is unset.

**Normative wire shapes:** [chat-storage WIRE_SCHEMAS.md](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md). **E2E crypto:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md).

## Service resolution

[`CreateServiceClients`](../src/base/net/ServiceClientFactory.cpp) picks an implementation per endpoint:

1. `base_url` set → HTTP client (`HttpRelayClient`, etc.)
2. else in-process mock (dev default)

Native messaging code (`P2pMessagingService`, `MessagingTools`) always calls `IRelayClient` / `IDirectoryClient` / `IRegistrationClient`; the factory swaps implementations underneath. See [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md).

**Baseline gap:** `IRelayClient` exposes `Send`, `PollInbox`, and `FetchChatHistory`. HTTP relay history uses signed `POST /api/relay/v1/streams/messages/query` — see [WIRE_SCHEMAS § Stream history](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#stream-history-http-relay).

## Data model

### Thread

| Field | Description |
|-------|-------------|
| `id` | UUID; **local only** — not on wire (D056) |
| `kind` | `ai`, `direct`, `group` |
| `participant_contact_ids` | One peer for direct; N for group (future) |
| `unread_count` | Sidebar badge |
| `preview` | Last message snippet |

**Target (v2b+):** `channel` (`e2e` \| `e2e_public`) on direct threads — see [DESIGN § Three chat tiers](../projects/chat-storage-and-memory/DESIGN.md#three-chat-tiers-d089).

### ThreadMessage

| Field | Description |
|-------|-------------|
| `sender_contact_id` | **Local rows:** `local:self`, `ai:assistant`. **Wire / peer rows:** communicating identity value (D079, D082), e.g. `relay:abc123` |
| `content_rml` | Rendered assistant blocks (optional; **local AI only** — never from wire, D030) |
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

**Target (v2a+):** see [chat-storage-and-memory DESIGN.md](../projects/chat-storage-and-memory/DESIGN.md) — `profile.db` (`threads` + `outbox` + `chat_targets`) + per-thread `thread.db`; no `index.json`.

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

## Relay envelope (target — D056, D090)

**No `thread_id` on the wire.** All direct tiers use **`body.e2e.payload_b64`** (AEAD ciphertext). Reject `public_relay`, `body.content_b64`, flat `body.text`, and legacy `thread_id`.

Full spec: [WIRE_SCHEMAS § RelayEnvelope](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#relayenvelope-v1--envelope_version-1).

```json
{
  "envelope_version": 1,
  "message_id": "550e8400-e29b-41d4-a716-446655440000",
  "sender_relay_id": "relay:alice123",
  "sender_contact_id": "relay:alice123",
  "route": {
    "kind": "direct",
    "channel": "e2e"
  },
  "sender_seq": 42,
  "session_epoch": 1,
  "body": {
    "e2e": { "payload_b64": "…" }
  },
  "timestamp": 1719662400123,
  "signature": "base64-ed25519"
}
```

**Private direct:** `"channel": "e2e"`. **Public direct:** `"channel": "e2e_public"` (same body shape).

**AEAD plaintext** is binary **ChatPayload** (D087/D090) — not JSON `body.content`. See [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md).

Inbound routing: `ChatTargetKey { peer_identity_kind, peer_identity_value: sender_contact_id, channel }` → local thread lookup (**private `e2e`:** find-only, D062; **`e2e_public`:** auto-create after decrypt, D080).

**Signature verify (E014, E016):** Inbound messages are verified with **`EnvelopeSigner::Verify`** using the sender's Ed25519 public key from **`PeerSigningKeyStore`** (directory at add-contact; lazy `GET /v1/users/{relay_user_id}` on cache miss). See [e2e DECISIONS E016](../projects/e2e-message-crypto/DECISIONS.md#e016--peer-signing-keys-relay-directory-source-local-cache-oob-fingerprint-at-add).

**Wire cutover (D063):** v2a-p2p ships this final envelope shape. v4 adds ChatPayload **validation** only — no second wire break. See [DESIGN § Wire cutover phasing](../projects/chat-storage-and-memory/DESIGN.md#wire-cutover-phasing-d063).

**Baseline code** still uses legacy `RelayEnvelope` with `thread_id` and `body.text` — replaced in v2a-p2p.

Local store is written **before** send. Server rejections do not delete history. **Unsent/failed** rows stay local — user **retries send**; **peer sync** (`FetchChatTargetMessages`, D058) fetches **missing messages from the peer**, not your pending outbox.

## E2E history sync (v6 — D058–D060)

| Mode | Trigger |
|------|---------|
| Tail sync | Open E2E thread, reconnect |
| Gap repair | Automatic on seq hole |
| User sync | Thread menu **Sync with peer** (D059) |

**Transport:** libp2p peer-direct `/pp-browser/chat-history/1.0.0` first; relay `POST /api/relay/v1/streams/messages/query` fallback (client maps `ChatHistoryRequest` → `stream_key` / `order_key`). Full spec: [WIRE_SCHEMAS § Stream history](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#stream-history-http-relay).

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

`ThreadKind::Group` and `participant_contact_ids[]` are reserved. Wire shape open (O008). Adding groups does not require a new local message schema.
