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
{data_dir}/machine.json               # device-wide: active profile, window, safe area, display; collect_run_logs, collect_crash_logs (optional bools, absent = off: "Collect run logs", "Collect crash logs"); update_card_version, update_card_dismissals, update_card_dismissed_ms (optional string / integer / integer, absent = never tapped: "Later" on the Chats page's new-version card, for that latest_client_version; no schema bump; an older build does not understand them and drops them the next time it saves the file, which is harmless: the card simply shows again)
{data_dir}/logs/pp-browser.log         # current launch log (0600; .1 … .N = previous launches: 7 days / 50 MB with collect_run_logs on, else only .1; .1 is always kept) — ops/CONFIGURATION.md § Log file; leaves the device only by Send run logs (test phase, contracts/DIAGNOSTICS.md)
{data_dir}/logs/metrics.log            # Metrics channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/logs/diag.log               # Diag channel: shareable lines (rotated like the log) — contracts/DIAGNOSTICS.md
{data_dir}/diagnostics/crash_pending.txt   # last fatal-signal dump (image_base=, raw frames, breadcrumbs); uploaded + cleared next launch
{data_dir}/profiles/{id}/
  manifest.json             # schema_version; backend = the backend key the profile belongs to (see Per-backend profiles)
  mesh_directory_nodes.json # last-good GET /v1/mesh/nodes (N027 cold start; schema_version 1; optional amp_wire / node_version / features, N032); per profile, so per backend
  mesh_node_reputation.json # mesh nodes and operators this profile demoted for a contradicted claim (N032; schema_version 1; demoted_until_ms / last_strike_ms / strikes); local only, never sent
  mesh_last_good_relays.json # relays that last carried this device's circuits (call-path-resilience K015; schema_version 1; relays: ≤3 PeerIds, most recent first);
                            # plaintext like the directory cache — read at mesh start, before any PIN unlock; local only, never sent; optional
  our_relays.json           # My relays (multi-relay R3, MR006, MR008, SIGNED_FORMATS § 3.3): {schema_version: 3, relays: [{origin, relay_id? (`relay:…`, the relay id its address names: its relay link's,
                            # the build's / config's for the org's relay, our own card's on a linked device; absent only for a developer build's bare origin, or the org's
                            # in a build that names none; one that is not a relay id drops the entry), registration: {state registered|failed|not_registered|expired|unknown,
                            # reason? (failed: unreachable|refused|identity|other, worded by the page), expires_at?, card_issued_at?, registration_id? (MR007: the one a revoke names, additive),
                            # at_ms?}}]}, preferred first, ≤4; user data (a restore takes the account locker's `relays`), seeded once from the
                            # deployment's relay + relay.extra_base_urls (relay links; an unreadable file, or schema 1 or 2 (PeerIds, before the cutover), is seeded again; a newer one is refused and never written: the seed in memory, changes refused); plaintext like the
                            # cursors (the card already names these relays); written atomically on every change and every registration answer
  preferences.json
  relay_pins.json           # the relay id each relay origin of ours must prove, and the highest certificate per relay id (SIGNED_FORMATS § 3, § relay_pins.json below); plaintext, written atomically
  relay_inbox_cursor.json   # poll watermarks {schema_version: 1, account_id, cursor, cursor_relay?, relays?: {origin: cursor}, registrations?: {origin: registration_id}}:
                            # the preferred relay's (cursor_relay names it, additive; absent: the preferred one's), then our others'; a relay leaving My relays loses its cursor;
                            # registrations (additive, 2026-11-22, schema stays 1 (INVENTORY S3); always written, empty too; an older app ignores it and drops it when it writes): the
                            # registration each relay's cursor counts in — a fresh registration is a new mailbox numbered from the start, so a cursor kept for another one,
                            # or while it was not known, is dropped and that inbox read from its start; a file without the key drops every cursor once
                            # (RelayInboxCoordinator; the receive pipeline drops what it took already);
                            # no schema_version (a file from before it): 1; a newer one is not read (those inboxes fetch from their oldest message) and never written over
  vault.bin                 # PIN-wrapped DEK (created on first secrets unlock)
  grace.bin                 # PIN grace window: DEK under K_grace, AAD "grace|{id}" (iOS, Android 9+; only within a window — AT_REST_ENCRYPTION § PIN grace window)
  biometric.bin             # Face ID / Touch ID unlock: DEK under K_bio, AAD "biometric|{id}" (iOS; only while the switch is on — AT_REST_ENCRYPTION § Face ID / Touch ID unlock)
  identity.enc              # identity JSON under DEK AEAD (plaintext schema_version 4, also its floor: an older or unversioned file is an unlock error, wipe the profile; device ML-DSA-65 + account ML-DSA + account KEM; optional initiation_floor; optional registered_card_issued_at — the account card the relay last answered for at register/finish, SERVICE_ENDPOINTS; optional registration_id — the preferred relay's registration a revoke names, MR007, additive; optional recovery_seed_b64 — M023, AT_REST_ENCRYPTION § Identity plaintext)
  own_card.enc              # our account's newest account card under DEK AEAD (AAD purpose "own-card", plaintext schema_version 1: {schema_version, card}; IdentityStore):
                            # the copy that outlives Reset chats, the start-up recovery's reset and a profile.db too old to upgrade (all of which remove threads/
                            # with its peer_cards); only a newer card of ours replaces it; a card of another account is ignored (a link or restore deletes the file
                            # with the old DEK). Absent until our first card; profile reset removes it with the rest; a newer schema_version is refused and
                            # never written over (the card is read from peer_cards meanwhile) — AT_REST_ENCRYPTION § Own card plaintext, ACCOUNT_CARD § Main device
  secrets.enc               # typed model API key (llm_api_key) under DEK AEAD, AAD purpose "config-secrets", schema_version 1; absent when there are none (PB-034)
  initiation_billing.json   # per-peer initiation billing state (P001; `{schema_version: 2, peers: {account → row}}`; 2 is also its floor: an older, unversioned or flat file is refused; a file not read (that, a newer one, not JSON) is never written over: that run's rows live in memory; additive relay_floor_minor / relay_floor_card_issued_at: a relay's payment_required floor and the kept card it came after — SERVICE_ENDPOINTS § The stranger's price)
  payment_promises.json     # payment promise receipts (pricing P002 / P003; plaintext, the signed promises as received): {schema_version: 2, promises: [...],
                            # pending_inbound?: [...] — inbound receipts staged until Accept or Ignore}; 2 is also its floor: another version, or none, is refused,
                            # and a file not read is never written over (that run's receipts live in memory); optional
  account_locker.json       # account locker sync state: schema_version 1, highest_version_by_locker, contact_clock, device_names (plaintext; ACCOUNT_LOCKER.md
                            # § On disk); no schema_version (a file from before it): 1; a newer one is not read and never written over (that run keeps it in memory)
  attachment_suppressions.json # attachments the person deleted here, not fetched again (R020): {schema_version: 1, threads: {thread_id: [content hash hex]}};
                            # plaintext, written atomically; no schema_version (a file from before it): 1; a newer one, or one not read, refuses every change; optional
  contacts.json             # address book (schema_version 3: local + remote + overrides{}); another version, or none, is refused; optional
                            # remote.locator_relay: a QR locator's preferred relay address {origin, relay_id} (multi-relay R4, MR008, SIGNED_FORMATS § 3.3), its relay list
                            # until a card (additive; one without a relay id of the `relay:` form, a `peer_id` one included, is read as none)
  client_compat.json        # cached GET /v1/client-compat (TTL 6h; optional): {schema_version: 1, fetched_at_unix, document} with every field the
                            # reader knows (SERVICE_ENDPOINTS § Client compatibility: ranges, node floor, switches, the org relay's certificate); no schema_version (a file from
                            # before it): read as it is; a newer one is no cache and never written over
  cache/icons/{key}/        # profile photos (ProfileIconCache): key `self` (ours) or an Account ID with `/ \ :` made `_`; meta.json {url, blob_id, kind,
                            # fetched_at_ms, local_filename} beside icon-<hash>.<ext> (named after its bytes); a cache, fetched again when its url or blob_id
                            # changes or the file is gone; plaintext, unversioned; optional
  home_brief.json           # phone Home "Today's briefs": the article feed's general headlines (schema_version 1; fetched_at_ms,
                            # feed cn|en, items[] of title / category (the site) / time_ms / url); plaintext, refetched when over
                            # 3 h old while the widget is on; device-only, never synced; optional
  threads/
    profile.db              # thread catalog, outbox, chat_targets (PSK + preview_enc encrypted; user_version 9, floor 9 — § Schema
                            # versions; restart_pending: a public session kept through a chat reset — Reset chats, or a profile.db below its floor — starts its next epoch on the next send);
                            # thread_local_prefs (pin / mute / archive per thread — device-only, plaintext)
                            # removed_messages (message_id PRIMARY KEY, removed_at unix ms) — the ids of the messages of a 1:1 or group
                            # chat cleared or deleted here (rows newer than 120 days), written before the rows go; a copy that comes
                            # again (a relay mailbox read from its start, a sibling's sent copy, a peer's history) is dropped before the
                            # receive pipeline and never stored again. Ids only, plaintext like the other metadata; pruned 91 days after
                            # removed_at (the relay keeps a record 90 days). Device-only; created on open (additive, no version step);
                            # gone with Reset chats and Reset my profile. Delete-for-me / for-everyone keep their row as a tombstone instead
                            # thread_mentions (thread_id, message_id, created_at; PRIMARY KEY (thread_id, message_id)) — a live group message
                            # that mentions this account, unread: a row on arrival (or when a held mention is decided, G015), all of a thread's
                            # rows dropped when it is opened, a message's row when that message is deleted (for everyone or here), the thread's
                            # when it goes; never written by history sync; per device. "You were mentioned" and jump-to-mention read it (additive)
                            # thread_mentions_read (thread_id PRIMARY KEY, message_id, created_at) — the newest read mention of a thread: written
                            # when the thread is opened (its newest unread row, if newer), dropped with that message or the thread / its
                            # history; Home's Mentions widget reads the newest over both tables to stay up 7 days (additive)
                            # thread_local_prefs.mute_mentions INTEGER NOT NULL DEFAULT 0 (1: while the thread is muted, mentions are silent too;
                            # G015.)
                            # thread_local_prefs.share_ai_confirmed INTEGER NOT NULL DEFAULT 0 (1: "don't ask again" before sharing this chat
                            # with the AI; cleared by the session menu or clearing the chat.)
                            # peer_cards (checked account cards per account — public keys, devices, relays and floor, plaintext, additive; ACCOUNT_CARD.md)
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
                            # first members.)
                            # group_invite_links (link_id PRIMARY KEY, group_id, secret_enc, maker, expires_at, max_uses, created_at;
                            # group-chat G021): the invite links this device made, kept to show again (text, QR) — secret_enc the
                            # link's 16-byte secret under the profile DEK (FileCipher, AAD purpose "group_link_secret"), the rest
                            # plaintext. Answering a join needs only the roster (the secret's hash); a row also lets a join for a
                            # link the roster no longer names be refused as revoked / expired. Not in the account locker (another
                            # device of the account answers by the roster, and cannot show the link again). Additive table
                            # pending_group_invites.owner_identity TEXT NOT NULL DEFAULT '' (G019: the group's owner a member's invite
                            # names, which the join bundle's statement must verify for; empty: the inviter. outbound = 1 for an invite
                            # this device sent, as owner or member.)
                            # relay_grants (direction, peer_account, recipient_account, sender_account, issued_at, expires_at, signature,
                            # confirmed; PRIMARY KEY (direction, peer_account); multi-relay MR005): `received` — the grant each account gave
                            # us, attached to relay sends to it; `issued` — the grant we gave each account, confirmed once it acked.
                            # Plaintext (signed statements the relay sees anyway). Not in the account locker: each device asks and issues
                            # for itself. Additive table — ACCOUNT_CARD § Relay grants
    {thread_id}/
      thread.db             # messages.content_enc, memory.value_enc encrypted; sync_state + metadata plaintext (user_version 4, floor 4 — D102, § Schema versions)
                            # messages.content_type 'deleted' = "delete for me" tombstone: id / seq / order kept, body = empty text (D103)
                            # An edited text (D106): the row is rewritten in place (same id / seq / order / timestamp, the new text, no spans)
                            # and its payload_json (inside content_enc) carries edited_at (the applied edit's send time, ms) and edited_by
                            # (the applied edit's message id); no column, user_version unchanged. Its `edit` annotation rows stay; all but
                            # the applied one are blanked (value / text empty), and all of them once the message is deleted.
                            # An edit of ours blanked before it went out keeps its row, seq and delivery state; no further
                            # send of it starts (only a try already on its way can still arrive).
                            # Local-only System rows (sender local:self, delivery 'local', never sent or synced; text empty, the UI words
                            # them from the payload): payload_json {control_type: "receive_failure"|"send_failure", detail: <JSON string>}.
                            # `send_failure` (#182): a message of ours did not go out, one row per message, id "send-failure-<message_id>"
                            # (+ "-group" for a partial group send); detail {reason, message_id}, reason = a SendFailureWord
                            # (mesh_off, no_address, ..., relay_rejected, recipient_not_found, sender_not_registered) or "group_members_missed";
                            # a reader that does not know a word shows the generic reason. It does not touch the thread's preview/order.
                            # `call_blocked`: 1:1 calls "who can call me" dropped, one row per caller in the DM, id "call-blocked:<caller>",
                            # rewritten in place per call (its time never moves back); detail {count, setting ("contacts_only"|"nobody"),
                            # call_ids (last 20 counted)}; text = the words at write time, a preview stand-in (the transcript words the payload);
                            # generation "local_action" (never model context); no unread (DEV/PLAN-call-stranger-declined.md).
```

### Content CAS

[content-cas](../../projects/content-cas/) (C001–C011): `CasStore` on disk; the attachment durable path is private CAS only (P2 / C007); legacy thread `blobs/` support removed.

Durable bytes live in a profile-level CAS with **two realms**. Chat decrypt never writes the public realm. Block files use **two-level hex sharding** (C010).

```
{data_dir}/profiles/{id}/
  cas/
    private/blocks/{aa}/{bb}/{content_id_hex}  # DEK-wrapped (PPBA)
    public/blocks/{aa}/{bb}/{content_id_hex}   # clear published bytes (v1)
  object_index.db       # cas_objects: realm, content_id, mime, filename, byte_length, published_from?, pinned, created_at (SqliteObjectIndex);
                        # user_version 1, also its floor: 0 (a file from before it) is taken as 1, its rows kept; a newer one is refused and left alone
  threads/{thread_id}/
    blobs_view/         # optional session plaintext materialization (not source of truth); wiped at start and on ClearDek;
                        # a file above 4 MiB appears here only after an explicit Open; {hash}.poster.jpg = a video's poster,
                        # a sound file's cover, or a picture's preview (long side <= 640 px: what its chat bubble shows)
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
| `own_card.enc` | Our account's newest card under DEK AEAD (beside the identity, outside the chat store) |
| `preferences.json` → `pin_is_default` | Schema v4; set when user chooses default PIN (“Just continue”); cleared on Change PIN |
| `preferences.json` → `pin_digits` | Optional (no schema bump); the PIN's kind, not a secret: absent = unknown, `0` = text PIN, `4`–`8` = digit PIN of that length (the phone gate's number pad). Written after each unlock with an own PIN and on Change PIN |
| `preferences.json` → `auto_renew_registration` | Schema v4; default true; renew near/past expiry after unlock |

Interactive unlock / chooser UX: [AT_REST_ENCRYPTION.md](AT_REST_ENCRYPTION.md). Forgotten PIN → wipe the profile directory.

Flat `threads/index.json` and `{thread_id}.json` from before the SQLite store are not read (nor removed); plaintext `identity.json` is not migrated — wipe the profile and recreate ([chat-storage D016](../../projects/chat-storage-and-memory/DECISIONS.md)). Overview: [COMPATIBILITY.md](COMPATIBILITY.md).

### Per-backend profiles

Each backend (Me → Network → Service, `backend.env`) has its own profile, so identities, contacts, conversations and caches of different backends never mix, and switching back finds everything as it was. A profile's `manifest.json` records its `backend` key:

| Backend | Key in a store build | Key in a tester build |
|---------|----------------------|-----------------------|
| `prod` | `prod` | — (never: runs as `staging`, else `dev`, else the custom backend) |
| `staging` (and a saved `qa`) | `staging` | `staging` |
| `dev` | `dev` | `dev` |
| `custom`, or any run with `PP_BROWSER_ORIGIN` | `custom-<12 hex>`: FNV-1a 64 of the normalized registration URL (origin for `PP_BROWSER_ORIGIN`) | `test-custom-<12 hex>`: the same hash |

**Per build kind** (release-compatibility [RC022](../../projects/release-compatibility/DECISIONS.md#rc022--tester-builds-never-touch-production-profiles-hard-guard)). A *store build* is a release or release candidate (the store binary); a *tester build* (`PP_TESTER_BUILD`, [BUILD § Backend origin](../ops/BUILD.md#backend-origin)) is an `internal/<n>` TestFlight upload, a tester APK or a desktop tester package. On iOS TestFlight and the App Store share one app and one data container (Android likewise under one package id), so both kinds read the same `{data_dir}`. A tester build never opens or changes a store build's profile: its keys are never `prod` or `custom-…` (a store user's custom profile at the same URL is `custom-<hash>`, the tester's `test-custom-<hash>`), and after selecting it refuses to start on a profile whose `manifest.backend` is `prod` or `custom-…` — whichever way it was named, a `--profile` override included — before anything in it is read or written. A store build opens any profile but selects only its own keys, so it never picks a tester profile up either. Testers who want their prod data reinstall the store app. With neither staging nor dev compiled a tester build runs only the custom backend (the URLs saved in the config file; none: no backend) under `test-custom-<hash>`.

qa is retired (release-compatibility RC021) and staging is a new environment, not qa renamed (RC009 as amended): a saved `backend.env: "qa"` runs on staging with a new `staging` profile, and a profile recorded for `qa` is left as it is, never selected again.

At startup, after the config is loaded, the registry selects the active profile for the key: the active one when it matches; else the existing profile recorded for the key; else a new profile with the key as its id. An active profile whose manifest records no backend — a fresh install's `default`, or a profile just reset (the reset writes its manifest again without one) — takes the key; no other profile is ever claimed (release 1 starts clean, RC017). `manifest.backend` is never overwritten once set, and a newer manifest is refused, never written. A `--profile` override is not redirected. The backend is chosen once per run; changing it takes effect on the next start.

Saving a backend switch also removes the device's push bindings from the running backend ([SERVICE_ENDPOINTS.md](SERVICE_ENDPOINTS.md)); the other profile registers its own after the restart.

There is no account-switcher UI: the profile follows the backend. Use `--profile NAME` for dev isolation. A `mesh_directory_nodes.json` left at the data root by older builds is no longer read.

### Shared root files

`config.json`, `profiles.json` and `machine.json` sit at the root, outside every profile, and every build on the device reads them — a store build and a tester build alike (RC022), and an older store build after a newer tester build. So their versions (`config_version`; `schema_version` of `profiles.json` and `machine.json`) **change only in a release**: a tester build may only add **optional** keys, which an older build ignores on read and drops the next time it saves the file (so a key a tester build adds must mean nothing harmful when absent). A store build reads a `profiles.json` holding a tester build's entries (`staging`, `dev`, `test-custom-…`) and its `active_profile_id` as any other: it keeps the entries and selects its own profile. The saved `backend.env` is not rewritten by a build that cannot run it (a tester build reading `prod`, a store build reading a staging it has no origin for): that build runs its fallback, and a save writes the file's choice back; only a switch in Me → Network replaces it. `SharedRootFileVersionsTest` pins each file's current version: bumping one is a deliberate change of that test, in a release.

### relay_pins.json

The relay id each relay this profile signs for must prove, and the highest valid certificate seen per relay id
(release-compatibility SIGNED_FORMATS § 3.2-3.4; [SERVICE_ENDPOINTS § Pins](SERVICE_ENDPOINTS.md#pins)): the relay ids
we expect, from each relay's address, never from its own answer. Per profile, so per backend; a pp-node keeps one in
its data directory, the same format.

```json
{ "schema_version": 3,
  "pins": [ { "origin": "https://www.brief.global/api/relay", "relay_id": "relay:…", "pinned_at_ms": 1761235200000,
              "mismatch": "other_relay", "presented_relay_id": "relay:…", "mismatch_at_ms": 1761235300000 } ],
  "certificates": [ { "relay_id": "relay:…", "sequence": 2, "certificate_b64": "…", "kept_at_ms": 1761235200000 } ] }
```

- `origin`: the relay base URL as relays are compared (`RelayOriginKey`: scheme and host lower case, the default port
  and a trailing `/` dropped). One pin per origin; a relay id may be pinned at several origins (its certificate lists
  up to four).
- `relay_id`: what its address names — the relay link it was added by (and `our_relays.json`), the build
  (`PP_RELAY_ID_PROD` / `_STAGING` / `_DEV`) or the config (`relay.relay_id`, `registration.relay_id`) for the org's origins, our
  own card on a linked device; only a developer build's bare origin, and the org's in a build or config that names
  none, take their first valid certificate's.
- `mismatch` / `presented_relay_id` / `mismatch_at_ms`: optional — when it was last checked the origin did not prove its
  address: `other_relay` (another relay's certificate, or another relay id named; `presented_relay_id` the one named),
  `no_valid_certificate`, or `origin_not_listed` (a valid certificate that does not list this origin). Present,
  nothing is signed for it. It goes when a later check finds the origin proving its address again, or the address
  changes (a new link, a build naming another relay id).
- Only our relays and the org's origins are pinned (a contact's card or QR code never writes a pin). At most 32;
  when full, the oldest pin that is neither one of our relays nor the org's origins (one removed from My relays since)
  goes.
- `certificates`: per relay id the highest valid certificate seen (standard base64 of the blob, kept byte for byte),
  from the build, `client-compat` or the relay's `relay-identity` — ours, the org's and contacts' relays alike (a
  contact's relay's is cached, never pinned). A lower one never replaces it. At most 32; when full, the oldest of a
  relay no pin names goes. Each is verified again against its relay id when the file is read (expiry aside: it is read
  where the certificate is used); one that does not verify is dropped, logged. `sequence` is informational.
- Another `schema_version` (1, 2: PeerId pins from before the cutover), or a file that is not JSON, is refused and read
  as empty (logged as an error): each relay is expected again from its address, and its certificate fetched again. A
  newer one is never overwritten: that run's pins are kept in memory only.

## Schema versions

**SQLite: a ladder per database** ([chat-storage D105](../../projects/chat-storage-and-memory/DECISIONS.md#d105--disk-schemas-move-up-a-ladder-from-a-floor)). `profile.db` and each `thread.db` say their schema in `PRAGMA user_version`; `SqliteMigrations` (`lib/storage`) brings one to this build's when it opens (`SqliteChatDbLadders`: `ProfileDbLadder`, `ThreadDbLadder`), before any store uses it. It opens **after unlock**: the thread store gets the profile's data key, opens profile.db and runs its ladder with the key in the step context (a step may rewrite encrypted columns); before that nothing opens, creates or migrates either database ([AT_REST_ENCRYPTION § Unlock flow](AT_REST_ENCRYPTION.md#unlock-flow) step 8):

| Found | What happens |
|-------|--------------|
| `0` (a new file) | The current schema is created and the version set |
| The current version | Nothing |
| Between the floor and the current version | The file is copied beside itself (`*.pre-migration`, SQLite's backup API, so the write-ahead log is in it); each step runs in its own transaction, which sets `user_version` to its target at its end; the copy is deleted. A step that fails is rolled back and the copy restored: the database stays at the version it had and stays shut; the app shows the start-up recovery ("Your chats could not be upgraded": Reset chats…) |
| Below the floor | Too old to upgrade: the store is reset — `profile.db`: the whole chat store (`threads/`, as Me → Storage → Reset chats; the account, contacts and settings stay) but its public sessions, which Reset chats keeps too: the `e2e_public` sessions with a key (account and device pair, `SessionsAChatResetKeeps`) are read from the old file before it goes — straight from its tables, whatever columns its version has, their keys still sealed (the AAD names the profile, not the file) — and written into the new one `restart_pending`; if that fails, none are kept and the reset goes on; a `thread.db`: that chat's messages (its place in the catalog stays). An error line is logged and `chat_store_reset` is written beside `threads/`; the app takes it once after start-up and says "your chats were reset" |
| Newer than this build's | Left alone and shut; the app shows the start-up recovery ("This data is from a newer version of the app": update the app, or Reset chats…) |

Today: `profile.db` 9 (floor 9: the signed-format cutover, release-compatibility SIGNED_FORMATS § 2.4–2.5, moved it with roster statements v4, roster changes v2 and relay grants v2), `thread.db` 4 (floor 4). **Release 1 is every store's start** ([release-compatibility RC017](../../projects/release-compatibility/DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1)): every tester deletes the app and installs release 1, so each floor is release 1's schema and no step from before it is kept (the baseline, the Account ID and the account card steps went with it); a pre-release file is below the floor. A store still creates a table that is new to it (`CREATE TABLE IF NOT EXISTS`, at the current shape); any change to a table that may already exist is a ladder step, never an `ALTER` on open.

A card of our own account is kept in `peer_cards` only from our own devices (`self`, `sibling`), or as `OwnCards` got it
back from our relays' directories after checking it against our own account keys (`recovered`,
[ACCOUNT_CARD § Main device](ACCOUNT_CARD.md)): one carried back by a contact, a directory or a relay any other way is
refused (`AccountKeyResolver`), so nothing relabels our record.

**Changing a schema:** bump the version, add the step from the old one (it must run on every shape that version had), keep the floor. The **floor** is the oldest version the latest app still upgrades. Phones install only the latest app — a user cannot step through the versions in between on iOS or Android — so the latest app carries every step from the floor up; the floor moves only at a milestone (a release a user is expected to have opened), and moving it resets the chats of anyone below it. These migrations are this device's own data: they are not a bridge to older peers ([COMPATIBILITY § No bridges](COMPATIBILITY.md#no-bridges-pre-release)), and no step reads an older wire form.

**JSON stores** include `schema_version` (or `config_version` for config) and have one version each: a file of an unsupported version is refused with a clear error — a newer one always, an older one below the store's floor (each its current version since release 1: `contacts.json` 3, `identity.enc` 4, `own_card.enc` 1, `initiation_billing.json` 2; none has a step), while the config and the preferences take an older version as is (their keys are optional). A change follows the same rule as the SQLite ladder: bump the version and add a step in the store's loader from the version below, keeping a floor.

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
| `call_invite_policy` | `preferences.json` | string, `contacts_only` \| `everyone` \| `nobody`; default `everyone` (Kenneth, 2026-11-11). Who may call this profile |
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
| `recovery_reminder_dismissals` | `preferences.json` | integer 0–2, optional (added after schema v14, no version bump); default 0. How often the Chats page's "save your recovery phrase" card was put off ("Later"); the card returns once, 7 days after the first "Later", and never after the second. Ignored once `recovery_phrase_seen` is true. Device-only, never synced or sent |
| `recovery_reminder_dismissed_ms` | `preferences.json` | integer (Unix ms), optional (added after schema v14, no version bump); default 0. When "Later" was last tapped on that card. Device-only, never synced or sent |
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
