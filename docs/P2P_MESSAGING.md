# P2P messaging

Person-to-person chat in pp-browser uses a **foundation-first** architecture: one `ThreadMessage` model for AI home, direct, and future group threads; local persistence as source of truth; HTTP relay/directory/registration transport with mock fallback when `base_url` is unset.

**Normative wire shapes:** [chat-storage WIRE_SCHEMAS.md](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md). **E2E crypto:** [MESSAGE_ENCRYPTION.md](MESSAGE_ENCRYPTION.md).

## Service resolution

[`CreateServiceClients`](../src/base/net/ServiceClientFactory.cpp) picks an implementation per endpoint:

1. `base_url` set → HTTP client (`HttpRelayClient`, etc.)
2. else in-process mock (dev default)

Native messaging code (`P2pMessagingService`, `MessagingTools`) always calls `IRelayClient` / `IDirectoryClient` / `IRegistrationClient`; the factory swaps implementations underneath. See [SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md).

**Relay history (D027):** `IRelayClient::FetchChatHistory` — `HttpRelayClient` uses signed `POST …/v1/streams/messages/query`; mock when `base_url` unset. See [WIRE_SCHEMAS § Stream history](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#stream-history-http-relay). Live integration tests ([D093](../projects/chat-storage-and-memory/DECISIONS.md#d093--relay-backend-for-v6-sync-d027)) run when these env vars are set:

| Variable | Purpose |
|----------|---------|
| `PP_BROWSER_RELAY_INTEGRATION_URL` | Relay base URL |
| `PP_BROWSER_RELAY_INTEGRATION_REQUESTER` | Requester `relay_user_id` |
| `PP_BROWSER_RELAY_INTEGRATION_PEER` | Peer `relay_user_id` for history query |
| `PP_BROWSER_RELAY_INTEGRATION_SIGN_KEY_HEX` | Ed25519 seed (hex) for relay API signing |

CI uses mock only; `pp_browser_relay_live_integration_test` skips when env is unset.

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

**Private `e2e` encrypt:** outbound send and inbound poll decrypt via `E2eRelayPayloadCodec` (c2). **`e2e_public`** remains plaintext on wire until c3 auto-key.

Local store is written **before** send. Server rejections do not delete history. **Unsent/failed** rows stay local — user **retries send**; **peer sync** (`FetchChatTargetMessages`, D058) fetches **missing messages from the peer**, not your pending outbox.

## E2E history sync (v6 — D058–D060)

| Mode | Trigger |
|------|---------|
| Tail sync | Open E2E thread, reconnect (`TailSync` — fetches seq > `loaded_max_seq`) |
| Gap repair | Automatic on seq hole; **Retry sync** banner (D059) |
| User sync | Thread menu **Sync with peer** — tail + gap repair + one older-history page (D059) |
| Scroll backfill | **Load older messages** banner at transcript top (D052/post-v6c) |

**Transport:** libp2p peer-direct `/pp-browser/chat-history/1.0.0` first; relay `POST …/v1/streams/messages/query` fallback (client maps `ChatHistoryRequest` → `stream_key` / `order_key`). Full spec: [WIRE_SCHEMAS § Stream history](../projects/chat-storage-and-memory/WIRE_SCHEMAS.md#stream-history-http-relay).

Per-message **Direct / Relay / Local** badges read the persisted `transport` column (post-v6d).

## AI-centric UX

- **No dedicated search UI** — use agent tools: `search_people`, `list_contacts`, `list_conversations`, `open_conversation`, `start_conversation`.
- Results render as `long_list` blocks with **Message** / **Add contact** chips (`send_chat_action` + JSON `payload`).
- **Registration** via `register_user` and `update_profile_nickname` tools (no sidebar banner).

## @ai in direct threads

Composer: `Message… or @ai ask assistant` (max length `kMaxComposeTextBytes`).

| Mode | Prefix | Relay |
|------|--------|-------|
| Local assist | `@ai …` | No — `relay_visible=false`, local `ai:assistant` row |
| Shared reply | `@ai+ …` | Yes — user prompt relayed; AI reply sent with `generation=ai_on_behalf` |
| Shared full | `@ai++ …` | Yes — user prompt + full AI reply relayed to peer |

Shared modes show a one-time confirm dialog before first send. Parser: `AtAiParser` (`@ai share`, `@ai share all` aliases).

Local `@ai` uses `AgentSession::SubmitScopedAssist` with thread transcript context.

## Modules

| Path | Role |
|------|------|
| `src/feature/messaging/MessagingHub.*` | Wiring, lifecycle |
| `src/feature/messaging/InboxController.*` | Active thread, display rows |
| `src/feature/messaging/P2pMessagingService.*` | Send, poll, dedup, sync UX |
| `src/feature/messaging/ChatSyncService.*` | `FetchChatTargetMessages`, tail/gap/user sync (D058–D059) |
| `src/feature/messaging/RelayReceivePipeline.*` | Inbound verify + classifier + backfill ingest |
| `src/feature/messaging/MessageRouter.*` | Composer routing |
| `src/feature/messaging/ContactActionDispatcher.*` | Chip payloads |
| `src/feature/ai/tools/MessagingTools.*` | Agent tool definitions |

## Group chat (future)

`ThreadKind::Group` and `participant_contact_ids[]` are reserved. Wire shape open (O008). Adding groups does not require a new local message schema.
