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

iOS keeps both under the app container's `Library/Application Support/pp-browser/` (SDL pref path); that directory is marked excluded from iCloud / iTunes backup at first use, so message history and keys never leave the device in a backup (PB-040). Android turns app backup off (`allowBackup="false"`) and excludes everything from cloud backup and device-to-device transfer (`res/xml/data_extraction_rules.xml`).

Override data root with `data_dir` in config (supports `~` expansion). How config is resolved and edited: [ops/CONFIGURATION.md](../ops/CONFIGURATION.md).

## On-disk layout

```
{config_dir}/config.json
{data_dir}/profiles.json
{data_dir}/machine.json
{data_dir}/logs/pp-browser.log         # current launch log (0600; .1 … .5 = previous launches) — ops/CONFIGURATION.md § Log file; never leaves the device
{data_dir}/logs/metrics.log            # Metrics channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/logs/diag.log               # Diag channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/diagnostics/last_upload.json    # {upload_id, at_ms} of the last Send diagnostics (0600)
{data_dir}/diagnostics/crash_pending.txt   # last fatal-signal dump (image_base=, raw frames, breadcrumbs); uploaded + cleared next launch
{data_dir}/profiles/{id}/
  manifest.json             # schema_version; backend = the backend key the profile belongs to (see Per-backend profiles)
  mesh_directory_nodes.json # last-good GET /v1/mesh/nodes (N027 cold start; schema_version 1; optional amp_wire / node_version / features, N032); per profile, so per backend
  mesh_node_reputation.json # mesh nodes and operators this profile demoted for a contradicted claim (N032; schema_version 1; demoted_until_ms / last_strike_ms / strikes); local only, never sent
  preferences.json
  relay_inbox_cursor.json   # poll watermark {relay_user_id, cursor}; delivery-queue ack progress
  vault.bin                 # PIN-wrapped DEK (created on first secrets unlock)
  grace.bin                 # PIN grace window: DEK under K_grace, AAD "grace|{id}" (iOS, Android 9+; only within a window — AT_REST_ENCRYPTION § PIN grace window)
  biometric.bin             # Face ID / Touch ID unlock: DEK under K_bio, AAD "biometric|{id}" (iOS; only while the switch is on — AT_REST_ENCRYPTION § Face ID / Touch ID unlock)
  identity.enc              # identity JSON under DEK AEAD (plaintext schema_version 3; device ML-DSA-65 + account ML-DSA + account KEM; pre-v3 wipe; optional initiation_floor; optional recovery_seed_b64 — M023, AT_REST_ENCRYPTION § Identity plaintext)
  secrets.enc               # typed model API key (llm_api_key) under DEK AEAD, AAD purpose "config-secrets", schema_version 1; absent when there are none (PB-034)
  initiation_billing.json   # per-peer initiation billing state (P001; schema_version 1)
  account_locker.json       # account locker sync state: highest_version_by_locker, contact_clock (plaintext; ACCOUNT_LOCKER.md § On disk)
  contacts.json             # address book (schema_version 2: local + remote + overrides{}; v2 = remarks that copied the nickname are cleared once on load); unversioned legacy migrates on load
  client_compat.json        # cached GET /v1/client-compat (TTL 6h; optional)
  home_brief.json           # phone Home "Today's briefs": the article feed's general headlines (schema_version 1; fetched_at_ms,
                            # feed cn|en, items[] of title / category (the site) / time_ms / url); plaintext, refetched when over
                            # 3 h old while the widget is on; device-only, never synced; optional
  threads/
    profile.db              # thread catalog, outbox, chat_targets (PSK + preview_enc encrypted; user_version 4;
                            # restart_pending: a public session kept through Reset chats starts its next epoch on the next send — additive);
                            # thread_local_prefs (pin / mute / archive per thread — device-only, plaintext, additive: no user_version bump)
                            # thread_mentions (thread_id, message_id, created_at; PRIMARY KEY (thread_id, message_id)) — a live group message
                            # that mentions this account, unread: a row on arrival (or when a held mention is decided, G015), all of a thread's
                            # rows dropped when it is opened, a message's row when that message is deleted (for everyone or here), the thread's
                            # when it goes; never written by history sync; per device. "You were mentioned" and jump-to-mention read it (additive)
                            # thread_mentions_read (thread_id PRIMARY KEY, message_id, created_at) — the newest read mention of a thread: written
                            # when the thread is opened (its newest unread row, if newer), dropped with that message or the thread / its
                            # history; Home's Mentions widget reads the newest over both tables to stay up 7 days (additive)
                            # thread_local_prefs.mute_mentions INTEGER NOT NULL DEFAULT 0 (1: while the thread is muted, mentions are silent too;
                            # G015. Added on open when missing — user_version unchanged)
                            # thread_local_prefs.share_ai_confirmed INTEGER NOT NULL DEFAULT 0 (1: "don't ask again" before sharing this chat
                            # with the AI; cleared by the session menu or clearing the chat. Added on open when missing — user_version unchanged)
                            # peer_cards (checked account cards per account — public keys and devices, plaintext, additive; ACCOUNT_CARD.md)
                            # peer_card_floors (account_id, issued_at — restored from the account locker: no older account card is taken)
                            # group_metadata.policy_json: {"preset": "open"|"managed"} (group-chat G017; older keys ignored)
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
                            # group_changes (group_id, change_id, change_json, received_at, line_shown; PRIMARY KEY (group_id,
                            # change_id); group-chat G018): the members' and admins' signed roster changes this device holds that the
                            # owner has not folded yet, the change JSON as received (with the signer's card when one rode it). The roster
                            # (group_rosters) is the held statement plus these; a change the statement folds, or one below its change
                            # floor, is deleted; an ended group (G016) keeps none. line_shown: the group's chat has the change's line.
                            # Not in the account locker (a restored device gets them from the member check, G013). Additive table
                            # group_rosters.added_by TEXT NOT NULL DEFAULT '' (G019: who brought the member in; empty for the group's
                            # first members. Added on open when missing — user_version unchanged)
                            # group_invite_links (link_id PRIMARY KEY, group_id, secret_enc, maker, expires_at, max_uses, created_at;
                            # group-chat G021): the invite links this device made, kept to show again (text, QR) — secret_enc the
                            # link's 16-byte secret under the profile DEK (FileCipher, AAD purpose "group_link_secret"), the rest
                            # plaintext. Answering a join needs only the roster (the secret's hash); a row also lets a join for a
                            # link the roster no longer names be refused as revoked / expired. Not in the account locker (another
                            # device of the account answers by the roster, and cannot show the link again). Additive table
                            # pending_group_invites.owner_identity TEXT NOT NULL DEFAULT '' (G019: the group's owner a member's invite
                            # names, which the join bundle's statement must verify for; empty: the inviter. outbound = 1 for an invite
                            # this device sent, as owner or member. Added on open when missing — user_version unchanged)
    {thread_id}/
      thread.db             # messages.content_enc, memory.value_enc encrypted; sync_state + metadata plaintext (user_version 2 — D102)
                            # messages.content_type 'deleted' = "delete for me" tombstone: id / seq / order kept, body = empty text (D103)
                            # Local-only System rows (sender local:self, delivery 'local', never sent or synced; text empty, the UI words
                            # them from the payload): payload_json {control_type: "receive_failure"|"send_failure", detail: <JSON string>}.
                            # `send_failure` (#182): a message of ours did not go out, one row per message, id "send-failure-<message_id>"
                            # (+ "-group" for a partial group send); detail {reason, message_id}, reason = a SendFailureWord
                            # (mesh_off, no_address, ..., relay_rejected) or "group_members_missed". It does not touch the thread's preview/order.
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

### PIN grace window (iOS Keychain / Android Keystore, outside the data dir)

One Keychain generic-password item per profile: service `dev.pp-browser.secure-blob`, account `{profile_id}/unlock_grace`, `kSecAttrAccessibleWhenUnlockedThisDeviceOnly`. It holds the key of `grace.bin` and the window's end on the device's own clocks, never the PIN or the DEK ([AT_REST_ENCRYPTION § PIN grace window](AT_REST_ENCRYPTION.md#pin-grace-window-gracebin--a-keychain-entry), [at-rest A013](../../projects/at-rest-crypto/DECISIONS.md#a013--pin-grace-window-a-pin-typed-a-short-while-ago-is-not-asked-again)). Deleted with `grace.bin` on expiry, Change PIN and a profile reset; one left by a deleted profile opens nothing (its `grace.bin` is gone).

Android 9+ (API 28+; nothing below): the same entry is a file `noBackupFilesDir/secure_blob/{hex sha256 of the name}` (the app's `no_backup` directory: never in a backup or a device transfer, deleted with an uninstall) = level (1 byte) | IV (12) | AES-256-GCM ciphertext + tag, AAD = level | name, under an AndroidKeyStore key: `pp_secure_blob_when_unlocked` (`setUnlockedDeviceRequired`, refused while the lock screen is up; the grace entry) or `pp_secure_blob_after_first_unlock` (`PpSecureBlob.java`).

### Face ID / Touch ID unlock (iOS Keychain, outside the data dir)

One Keychain generic-password item per profile: service `dev.pp-browser.secure-blob`, account `{profile_id}/unlock_biometric`, `kSecAttrAccessControl` = `kSecAttrAccessibleWhenUnlockedThisDeviceOnly` + `kSecAccessControlBiometryCurrentSet` (reading it shows the system Face ID / Touch ID prompt; a new enrolment makes it unreadable). It holds version 1 + the key of `biometric.bin`, never the PIN or the DEK ([AT_REST_ENCRYPTION § Face ID / Touch ID unlock](AT_REST_ENCRYPTION.md#face-id--touch-id-unlock-biometricbin--a-keychain-entry), [at-rest A014](../../projects/at-rest-crypto/DECISIONS.md#a014--face-id--touch-id-unlock-the-dek-behind-the-enrolled-biometry)). Deleted with `biometric.bin` when the switch is turned off, when the enrolment changed, and on a recovery-phrase reset, a transfer-code import or a profile reset; one left by an uninstall or a deleted profile opens nothing (its `biometric.bin` is gone).

### PIN-related files (disk only)

| File / field | Role |
|--------------|------|
| `vault.bin` | PIN-wrapped DEK |
| `grace.bin` | DEK under the PIN grace window's key (with the Keychain item / Keystore file above; deleted when the window ends) |
| `biometric.bin` | DEK under the Face ID / Touch ID unlock key (with the Keychain item above; present while the switch is on) |
| `identity.enc` | Identity JSON under DEK AEAD |
| `preferences.json` → `pin_is_default` | Schema v4; set when user chooses default PIN (“Just continue”); cleared on Change PIN |
| `preferences.json` → `pin_digits` | Optional (no schema bump); the PIN's kind, not a secret: absent = unknown, `0` = text PIN, `4`–`8` = digit PIN of that length (the phone gate's number pad). Written after each unlock with an own PIN and on Change PIN |
| `preferences.json` → `auto_renew_registration` | Schema v4; default true; renew near/past expiry after unlock |

Interactive unlock / chooser UX: [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md). Forgotten PIN → wipe the profile directory.

Legacy flat `threads/index.json` and `{thread_id}.json` are removed on first run after the SQLite migration (development wipe — see [chat-storage D016](../../projects/chat-storage-and-memory/DECISIONS.md)). Plaintext `identity.json` is not migrated — wipe the profile and recreate. Overview: [COMPATIBILITY.md](COMPATIBILITY.md).

### Per-backend profiles

Each backend (Me → Network → Service, `backend.env`) has its own profile, so identities, contacts, conversations and caches of different backends never mix, and switching back finds everything as it was. A profile's `manifest.json` records its `backend` key:

| Backend | Key (and id of a profile created for it) |
|---------|------------------------------------------|
| `prod` | `prod` |
| `qa` | `qa` |
| `custom`, or any run with `PP_BROWSER_ORIGIN` | `custom-<12 hex>`: FNV-1a 64 of the normalized registration URL (origin for `PP_BROWSER_ORIGIN`) |

At startup, after the config is loaded, the registry selects the active profile for the key: the active one when it matches; else the existing profile recorded for the key; else a new profile with the key as its id. A profile with no recorded backend (made before this field) is adopted in place as the key (as `prod` when the key is `qa`), so an existing user keeps their `default` profile as the prod one without moving data. `manifest.backend` is never overwritten once set. A `--profile` override is not redirected. The backend is chosen once per run; changing it takes effect on the next start.

Saving a backend switch also removes the device's push bindings from the running backend ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md)); the other profile registers its own after the restart.

There is no account-switcher UI: the profile follows the backend. Use `--profile NAME` for dev isolation. A `mesh_directory_nodes.json` left at the data root by older builds is no longer read.

## Schema versioning

All JSON stores include `schema_version` (or `config_version` for config). Unsupported newer versions fail with a clear error. Forward migrators can be registered for future v1→v2 changes during development.

**No legacy import:** older flat layouts (e.g. `identity.json` at data root) are not migrated. Delete the data directory when the layout changes during development. See [COMPATIBILITY.md](COMPATIBILITY.md) for dirty-folder and newer-peer policy.

## Preferences on disk

| Key | File | Notes |
|-----|------|--------|
| `appearance` | `preferences.json` | `system`, `light`, or `dark` |
| `language` | `preferences.json` | `system` or BCP-47 tag (`en`, `zh-Hans`); schema v6 |
| `pin_is_default` | `preferences.json` | boolean, schema v4 |
| `pin_digits` | `preferences.json` | optional int: absent unknown, 0 text PIN, 4–8 digit PIN; other values read as unknown |
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
| `attachment_download_policy` | `preferences.json` | string `smart` (default) \| `always_auto` \| `on_demand`; `always_auto` with both network booleans false reads back as `smart` |
| `attachment_auto_on_wifi` | `preferences.json` | boolean, optional; default true. With `always_auto`: download everything on Wi-Fi |
| `attachment_auto_on_cellular` | `preferences.json` | boolean, optional; default false. With `always_auto`: download everything on the mobile network (phones only) |
| `crash_reports_enabled` | `preferences.json` | boolean, schema v14 — opt-in upload of compact crash envelopes to `{relay}/v1/crash-reports` |
| `call_invite_policy` | `preferences.json` | string, `contacts_only` \| `everyone` \| `nobody`; default `everyone`. Schema v16 is a one-time reset: a file below schema v16 reads as `everyone` whatever it holds, and is saved back as v16, so a later choice is kept (Kenneth, 2026-11-11). Who may call this profile |
| `ai_dot_shown` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default true. Me → AI assistant → Show AI dot (phones) |
| `ai_dot_look` | `preferences.json` | string `a` (one eye) \| `b` (two eyes, default) \| `c` (cool) \| `d` (plain); anything else reads back as `b` |
| `ai_dot_size` | `preferences.json` | string `s` (52dp) \| `m` (64dp, default) \| `l` (76dp); anything else reads back as `m`. Me → AI assistant → Assistant size (phones) |
| `ai_dot_left` | `preferences.json` | boolean, optional; default false (right edge). The side the phone AI dot rests on, written after a drag |
| `ai_dot_y_permille` | `preferences.json` | integer 0–1000, optional; default 600. How far down its band (under the navigation bar, above the tab bar) the dot rests |
| `ai_dot_tucked` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default false. The phone AI dot is tucked half off its edge (flung or dragged into it) until dragged out. Device-only |
| `ai_panel_intro_seen` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default false. The ask-AI panel's one-time privacy card was dismissed ("Got it"). Device-only |
| `home_widgets_hidden` | `preferences.json` | string array, optional (added after schema v14, no version bump); default empty (all shown). Phone Home widgets removed in Edit Home: `favorites` \| `recent_chats` \| `calls` \| `mentions` \| `new_people` \| `brief` \| `security`; unknown ids (the retired `messages`) are ignored. A removed widget is not drawn or computed: a removed `calls` does not read `call_sessions`, a removed `brief` is not fetched. Device-only, never synced (like `thread_local_prefs`) |
| `home_widgets_order` | `preferences.json` | string array, optional (added after schema v14, no version bump); default empty (the default order: `favorites`, `calls`, `mentions`, `recent_chats`, `brief`, `new_people`). The phone Home widgets' order, set with Edit Home's drag handles; unknown and repeated ids are dropped, a widget it lacks goes at the end. Device-only, never synced (like `thread_local_prefs`) |
| `starred_contacts` | `preferences.json` | string array, optional (added after schema v14, no version bump); default empty. Contact ids starred on the contact page ("Add to Favorites"), in the order they were starred; empty ids and repeats are dropped on read, an id with no contact is ignored. They come first in the phone Home Favorites widget and the New chat page's frequent group. Device-only, never synced or sent (like `thread_local_prefs`) |
| `recovery_phrase_seen` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default false. Set once the user has opened their recovery phrase on Me → Security; the phone Home Security widget shows "Protected" after that, "Save recovery phrase" before. Never cleared by the app. Device-only, never synced or sent (like `thread_local_prefs`) |
| `unlock_grace_minutes` | `preferences.json` | integer, optional (added after schema v14, no version bump); one of 0 / 5 / 15 / 60 or -1 (Always: until the phone restarts, [at-rest A015](../../projects/at-rest-crypto/DECISIONS.md#a015--pin-grace-window-an-always-option-the-phones-own-lock-is-the-lock)), default 15; any other value reads back as 15. The PIN grace window (Me → Security → "Ask for PIN again after"): minutes after the last foreground use during which a cold start skips the PIN; 0 = every time. Only offered with an own PIN on a platform with a secure store. Device-only, never synced or sent |
| `home_show_dynamic` | `preferences.json` | boolean, optional (added after schema v14, no version bump); default false. Home shows the 动态 widgets section; default off = minimal AI-first Home (Me → Appearance → Show Activity on Home, phones). Device-only, never synced or sent (like `thread_local_prefs`) |

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
