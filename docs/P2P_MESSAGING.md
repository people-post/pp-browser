# P2P messaging

Person-to-person chat in pp-browser uses a **foundation-first** architecture: one `ThreadMessage` model for AI home, direct, and future group threads; local JSON as source of truth; HTTP relay/directory/registration as transport (mocked when `base_url` is empty).

## Data model

### Thread

| Field | Description |
|-------|-------------|
| `id` | UUID; used in relay envelope |
| `kind` | `ai`, `direct`, `group` |
| `participant_contact_ids` | One peer for direct; N for group (future) |
| `unread_count` | Sidebar badge |
| `preview` | Last message snippet |

### ThreadMessage

| Field | Description |
|-------|-------------|
| `sender_contact_id` | `local:self`, `ai:assistant`, or contact id |
| `content_rml` | Rendered assistant blocks (optional) |
| `relay_visible` | `false` for `@ai` assist (never relayed) |
| `delivery` | `local`, `pending`, `relayed`, `failed` |

Special IDs: `local:self`, `ai:assistant`.

## Persistence

Profile-scoped layout (see [CONFIGURATION.md](CONFIGURATION.md)):

```
{data_dir}/profiles/{profile_id}/
  identity.json
  contacts.json
  threads/index.json
  threads/{thread_id}.json
```

Configure endpoints via user config (`~/.config/pp-browser/config.json` on Linux) or in-app **Settings**:

```json
{
  "data_dir": "~/.local/share/pp-browser",
  "relay": { "base_url": "" },
  "directory": { "base_url": "" },
  "registration": { "base_url": "" }
}
```

Empty `base_url` uses in-process mocks.

## Relay envelope

```json
{
  "thread_id": "uuid",
  "message_id": "uuid",
  "sender_relay_id": "relay:…",
  "body": { "text": "…" },
  "timestamp": 1234567890,
  "signature": "…"
}
```

Local store is written **before** send. Server rejections do not delete history.

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
