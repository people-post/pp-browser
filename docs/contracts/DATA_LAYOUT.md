# Data layout (on-disk)

**Tier:** contract  
**Related:** [COMPATIBILITY.md](COMPATIBILITY.md), [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md), [ops/CONFIGURATION.md](../ops/CONFIGURATION.md), [architecture/PLATFORMS.md](../architecture/PLATFORMS.md).

pp-browser stores machine-wide connection settings separately from per-profile identity data. No first-run wizard is required: built-in platform defaults apply when no config file exists.

## Terminology

| Term | Meaning |
|------|---------|
| **Profile** | Network identity namespace (keypair, contacts, conversations) |
| **Session / thread** | One conversation in the sidebar (not an account) |
| **Machine** | One app install under one OS user |

## Paths (desktop)

| Scope | Linux | macOS | Windows |
|-------|-------|-------|---------|
| Config | `$XDG_CONFIG_HOME/pp-browser/config.json` or `~/.config/pp-browser/` | `~/Library/Application Support/pp-browser/` | `%APPDATA%/pp-browser/` |
| Data | `$XDG_DATA_HOME/pp-browser/` or `~/.local/share/pp-browser/` | `~/Library/Application Support/pp-browser/data/` | `%LOCALAPPDATA%/pp-browser/` |

Override data root with `data_dir` in config (supports `~` expansion). How config is resolved and edited: [ops/CONFIGURATION.md](../ops/CONFIGURATION.md).

## On-disk layout

```
{config_dir}/config.json
{data_dir}/profiles.json
{data_dir}/machine.json
{data_dir}/profiles/{id}/
  manifest.json
  preferences.json
  relay_inbox_cursor.json   # poll watermark {relay_user_id, cursor}; delivery-queue ack progress
  vault.bin                 # PIN-wrapped DEK (created on first secrets unlock)
  identity.enc              # identity JSON under DEK AEAD (plaintext schema_version 3; device ML-DSA-65 + account ML-DSA + account KEM; pre-v3 wipe; optional initiation_floor)
  initiation_billing.json   # per-peer initiation billing state (P001; schema_version 1)
  contacts.json             # address book (schema_version 1: local + remote + overrides{}); unversioned legacy migrates on load
  client_compat.json        # cached GET /v1/client-compat (TTL 6h; optional)
  threads/
    profile.db              # thread catalog, outbox, chat_targets (PSK + conversation KEM columns encrypted; user_version 3)
    {thread_id}/
      thread.db             # messages, memory, sync_state (plaintext — D048)
      blobs/                # attachment placeholder
```

### PIN-related files (disk only)

| File / field | Role |
|--------------|------|
| `vault.bin` | PIN-wrapped DEK |
| `identity.enc` | Identity JSON under DEK AEAD |
| `preferences.json` → `pin_is_default` | Schema v4; set when user chooses default PIN (“Just continue”); cleared on Change PIN |
| `preferences.json` → `auto_renew_registration` | Schema v4; default true; renew near/past expiry after unlock |

Interactive unlock / chooser UX: [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md). Forgotten PIN → wipe the profile directory.

Legacy flat `threads/index.json` and `{thread_id}.json` are removed on first run after the SQLite migration (development wipe — see [chat-storage D016](../../projects/chat-storage-and-memory/DECISIONS.md)). Plaintext `identity.json` is not migrated — wipe the profile and recreate. Overview: [COMPATIBILITY.md](COMPATIBILITY.md).

Phase 1 ships a single `default` profile. Use `--profile NAME` for dev isolation (no account-switcher UI yet).

## Schema versioning

All JSON stores include `schema_version` (or `config_version` for config). Unsupported newer versions fail with a clear error. Forward migrators can be registered for future v1→v2 changes during development.

**No legacy import:** older flat layouts (e.g. `identity.json` at data root) are not migrated. Delete the data directory when the layout changes during development. See [COMPATIBILITY.md](COMPATIBILITY.md) for dirty-folder and newer-peer policy.

## Preferences on disk

| Key | File | Notes |
|-----|------|--------|
| `appearance` | `preferences.json` | `system`, `light`, or `dark` |
| `language` | `preferences.json` | `system` or BCP-47 tag (`en`, `zh-Hans`); schema v6 |
| `pin_is_default` | `preferences.json` | boolean, schema v4 |
| `auto_renew_registration` | `preferences.json` | boolean, schema v4 (default true) |
| `show_notifications` | `preferences.json` | boolean; alerts ≠ sync |
| `reduce_transparency` | `preferences.json` | boolean, schema v8; opaque compact chrome |
| `compact_chrome_frost` | `preferences.json` | boolean, schema v8; default true; dogfood off in JSON |
| `reachability_nudge_acked_status` | `preferences.json` | string, schema v9; empty / `outbound_only` / `blocked` — Me → Network attention ack |
| `tool_permissions` | `preferences.json` | object, schema v11 — agent tool trust (`defaults` by risk, `by_tool`, `by_provider`; decisions `allow` \| `ask` \| `deny`) |
| `recent_emojis` | `preferences.json` | string array, schema v12 — MRU glyphs for the in-app emoji picker (cap 36) |

`tool_permissions` shape:

```json
"tool_permissions": {
  "schema_version": 1,
  "defaults": { "read": "allow", "write": "ask", "destructive": "ask" },
  "by_tool": { "add_contact": { "decision": "allow" } },
  "by_provider": {}
}
```

Stylesheet entry points (`foundation.rcss`, `components.rcss`, `colors-*.rcss`) and theme UX: [ui/UI_DESIGN_SYSTEM.md](../ui/UI_DESIGN_SYSTEM.md). The legacy `theme` path field in config remains for compatibility.
