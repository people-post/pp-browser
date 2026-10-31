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

iOS keeps both under the app container's `Library/Application Support/pp-browser/` (SDL pref path); that directory is marked excluded from iCloud / iTunes backup at first use, so message history and keys never leave the device in a backup (PB-040).

Override data root with `data_dir` in config (supports `~` expansion). How config is resolved and edited: [ops/CONFIGURATION.md](../ops/CONFIGURATION.md).

## On-disk layout

```
{config_dir}/config.json
{data_dir}/profiles.json
{data_dir}/machine.json
{data_dir}/mesh_directory_nodes.json   # last-good GET /v1/mesh/nodes (N027 cold start; schema_version 1)
{data_dir}/logs/pp-browser.log         # current launch log (0600; .1 … .5 = previous launches) — ops/CONFIGURATION.md § Log file; never leaves the device
{data_dir}/logs/metrics.log            # Metrics channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/logs/diag.log               # Diag channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/diagnostics/last_upload.json    # {upload_id, at_ms} of the last Send diagnostics (0600)
{data_dir}/diagnostics/crash_pending.txt   # last fatal-signal dump (image_base=, raw frames, breadcrumbs); uploaded + cleared next launch
{data_dir}/profiles/{id}/
  manifest.json
  preferences.json
  relay_inbox_cursor.json   # poll watermark {relay_user_id, cursor}; delivery-queue ack progress
  vault.bin                 # PIN-wrapped DEK (created on first secrets unlock)
  identity.enc              # identity JSON under DEK AEAD (plaintext schema_version 3; device ML-DSA-65 + account ML-DSA + account KEM; pre-v3 wipe; optional initiation_floor; optional recovery_seed_b64 — M023, AT_REST_ENCRYPTION § Identity plaintext)
  secrets.enc               # typed model API key (llm_api_key) under DEK AEAD, AAD purpose "config-secrets", schema_version 1; absent when there are none (PB-034)
  initiation_billing.json   # per-peer initiation billing state (P001; schema_version 1)
  account_locker.json       # account locker sync state: highest_version_by_locker, contact_clock (plaintext; ACCOUNT_LOCKER.md § On disk)
  contacts.json             # address book (schema_version 2: local + remote + overrides{}; v2 = remarks that copied the nickname are cleared once on load); unversioned legacy migrates on load
  client_compat.json        # cached GET /v1/client-compat (TTL 6h; optional)
  threads/
    profile.db              # thread catalog, outbox, chat_targets (PSK + preview_enc encrypted; user_version 4;
                            # restart_pending: a public session kept through Reset chats starts its next epoch on the next send — additive);
                            # thread_local_prefs (pin / mute / archive per thread — device-only, plaintext, additive: no user_version bump)
                            # thread_mentions (thread_id, message_id, created_at; PRIMARY KEY (thread_id, message_id)) — a live group message
                            # that mentions this account, unread: a row on arrival (or when a held mention is decided, G015), all of a thread's
                            # rows dropped when it is opened, a message's row when that message is deleted (for everyone or here), the thread's
                            # when it goes; never written by history sync; per device. "You were mentioned" and jump-to-mention read it (additive)
                            # thread_local_prefs.mute_mentions INTEGER NOT NULL DEFAULT 0 (1: while the thread is muted, mentions are silent too;
                            # G015. Added on open when missing — user_version unchanged)
                            # peer_cards (checked account cards per account — public keys and devices, plaintext, additive; ACCOUNT_CARD.md)
                            # peer_card_floors (account_id, issued_at — restored from the account locker: no older account card is taken)
                            # group_metadata.roster_statement_json / sync_state / sync_request_id / sync_requested_at (the owner-signed
                            # roster held and a restored group's member check — group-chat G012/G013, plaintext, additive)
                            # group_metadata.sync_state: active | checking | removed | dissolved. dissolved (G016): the owner ended the
                            # group — group_rosters empty, group_targets row gone, pending invites expired, roster_statement_json empty;
                            # the group's thread and messages stay (read-only). Terminal; no new column. The account locker keeps it as a
                            # tombstone (`left`, `dissolved`: terminal), so a restore does not bring it back
                            # group_pending_sends (group_id, recipient, control_type, detail_json, display; PRIMARY KEY (group_id, recipient,
                            # control_type); G016): the `group_dissolved` to each member and the `group_invite_refused` to each invitee still
                            # out, written in the SAME transaction that ends the group (roster emptied, send target dropped, invites voided,
                            # sync_state dissolved, the signed dissolve held in roster_statement_json) — a crash leaves all or none. A row is
                            # deleted once its copy reached the outbox; the rest are sent again when messaging comes up and after a failure
                            # (30 s, 2 min, 10 min). Additive table (CREATE IF NOT EXISTS)
    {thread_id}/
      thread.db             # messages.content_enc, memory.value_enc encrypted; sync_state + metadata plaintext (user_version 2 — D102)
                            # messages.content_type 'deleted' = "delete for me" tombstone: id / seq / order kept, body = empty text (D103)
```

### Content CAS (planned)

**Status:** design accepted — [content-cas](../../projects/content-cas/) (C001–C011). `CasStore` on disk; attachment durable path is private CAS only (P2 / C007); legacy thread `blobs/` support removed.

After cutover (C007 big bang), durable bytes move to a profile-level CAS with **two realms**. Chat decrypt never writes the public realm. Block files use **two-level hex sharding** (C010).

```
{data_dir}/profiles/{id}/
  cas/
    private/blocks/{aa}/{bb}/{content_id_hex}  # DEK-wrapped (PPBA)
    public/blocks/{aa}/{bb}/{content_id_hex}   # clear published bytes (v1)
  object_index.db       # realm, content_id, mime, pins, published_from?, thread refs
  threads/{thread_id}/
    blobs_view/         # optional session plaintext materialization (not source of truth); wiped at start and on ClearDek;
                        # a file above 4 MiB appears here only after an explicit Open
    # blobs/            # not used; durable bytes live in cas/private only
  picked/               # Android: <unique>/<name> = a copy of a content:// pick, made so decoders can read it; removed as soon as
                        # the attachment is prepared, and the whole folder at start (iOS keeps its own tmp/pp-picks)
  blob_cipher/
    outgoing/           # <id> = AEAD ciphertext of an attachment being sent, <id>.rec = its record (content key, fields,
                        # thread) sealed under the profile DEK. Removed when the message is sent, when the person
                        # discards it, and at start when older than 24 h (file mtime). Kept after a failed send so
                        # "Retry" (also after the app was killed) sends the same ciphertext with the same key.
    incoming/           # <content_hash>.part = a CDN download in progress (ciphertext only); deleted when read back
                        # or aborted, and the whole folder at start
  threads/ai-images/    # reserved name, NOT a thread (no thread.db): blobs_view/ only
    blobs_view/         # pre-send preview of the image the user is about to send to the AI; removed on send / discard, cleared at startup
                        # (sent AI images live in cas/private + the AI thread's own blobs_view, referenced by the message)
  threads/voice/        # reserved name, NOT a thread (no thread.db): blobs_view/ only
    blobs_view/voice-rec/  # the voice recording before it is sent: written when the recording ends (nothing on disk before, and a cancelled
                        # one leaves nothing), deleted once it has been read for the send (the staged ciphertext is the retry path),
                        # emptied when the next recording starts, cleared at startup and when the vault locks (like all blobs_view/)
```

| Realm | At rest | Object id | How it gets bytes |
|-------|---------|------------|-------------------|
| **private** | Profile DEK wrap (PPBA) | BLAKE2b-256(plaintext) — R016 | Chat/E2E decrypt → memory → wrap; never auto-public |
| **public** | Clear published payload (v1) | Hash(published bytes); new object on publish | Explicit **Share publicly…** only |

**Private presentation (C011):** process RAM LRU (`AttachmentPlaintextMemoryCache`) for small private plaintext while unlocked; wiped with `blobs_view/` on ClearDek. A private video above `AttachmentLimits::max_receive_bytes` is never downloaded, so it has no `blobs_view` copy; CAS ingest unchanged. (This step changes nothing on disk.) Public clear bytes need no DEK wipe path.

Delivery (peer blob OPEN, circuit, HTTP/CDN) stays orthogonal to realm.

### Retired: local account backup (iOS Keychain, outside the data dir)

Earlier builds kept a PIN-sealed account backup in one Keychain generic-password item (service `dev.pp-browser.secure-blob`, account `account-backup`). It is retired — nothing reads or writes it ([at-rest A011](../../projects/at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)); account state now lives in the encrypted account locker ([ACCOUNT_LOCKER.md](ACCOUNT_LOCKER.md)). Once messaging is up the app deletes that item if present (`RemoveRetiredKeychainBackup`), until app version 1.1.0 (`kRetiredKeychainBackupRemoveAt`).

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
| `sidebar_width_dp` | `preferences.json` | integer, optional (added after schema v14, no version bump); default 240; shell clamps to 200–480 dp. Expanded-layout secondary pane width |
| `sidebar_collapsed` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default false. Expanded-layout secondary pane collapsed |
| `auxiliary_width_dp` | `preferences.json` | integer, optional (added after schema v14, no version bump); default 320; shell clamps to 280–640 dp. Expanded-layout auxiliary pane width |
| `crash_reports_enabled` | `preferences.json` | boolean, schema v14 — opt-in upload of compact crash envelopes to `{relay}/v1/crash-reports` |

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
