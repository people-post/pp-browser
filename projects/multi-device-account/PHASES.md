# Multi-device account — phases

Ordering only. Spec: [DESIGN.md](DESIGN.md). ADRs: [DECISIONS.md](DECISIONS.md).

**Rollout note (2026-08-13):** Skip interim dogfood/wipe between Account-ID cut and multi-device; keep implementing until a coherent multi-device slice is ready to test once.

**Next:** per-device sessions (**M021**), phases **d3–d6** below (d1–d2 landed); account state (**M023**), phases **s1–s6**. m4c is **superseded by M023** (do not build it); m4d is folded into the d4 revoke.

## m0 — Design freeze

- [x] Account ID format (M002)
- [x] Account vs device keys; S1 signing (M003)
- [x] Shared DEK / per-device vault (M004)
- [x] Private PSK not auto-synced (M005)
- [x] Brief register binding (M006)
- [x] Hard-cut direction for wire identity (M007)
- [x] Contact/wire/directory ADRs (**M009–M011**); link-device deferred (**M012**)
- [x] Release scope B′ (**D100**)
- [x] Thin amend ADRs in chat-storage / e2e / at-rest
- [x] Project README + CURRENT_STATE
- [x] Post-m4b slice ADRs (**M016–M019**)

## m1 — Types and storage split

- [x] Account keypair + Account ID in identity model (separate from device key / Peer ID)
- [x] Persist account material under DEK; device key local to install
- [x] Profile/AAD story compatible with shared DEK across devices
- [x] Unit tests for Account ID encode/decode (M002) — `ml_dsa_test` + identity_store persist
- [x] Client register + `IdentityStore::SignBytes` / envelope verify use account ML-DSA-65 (hard cut)

## m2 — Brief directory (M011) then wire / thread hard cut

**Ship Brief API first (or same window), then client wire.**

### m2a — Brief (www)

- [x] `GET /v1/users/by-account/:account_id`
- [x] Search: top-level `account_id`; `ids[]` with `account` primary; **`q=` matches Account ID prefix, nickname, and `relay:`**
- [x] Route lookup `GET /v1/users/:relay_user_id` always returns `account_id` (when bound)
- [x] Update [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md)
- [x] Register finish echoes `account_id`

### m2b — Client wire / catalog

- [x] `ContactIdKind::Account` / `peer_identity_kind=account` (M009)
- [x] `ChatTargetKey` / envelope `sender_contact_id` / AAD → Account ID (M010)
- [x] Ingest verify against account signing key; signing cache keyed by Account ID
- [x] Directory `LookupByAccount` + add-contact / key register by Account
- [x] History/stream route remains `relay:` (resolve from contact)
- [x] Migrate or wipe pre-cut `relay:`-keyed local state (pre-release wipe OK — COMPATIBILITY)
- [x] Promote normative identity note in `docs/contracts/WIRE_SCHEMAS.md`
- [x] Call stack person identity → Account ID (participants, media stream ids, UI); PeerId/`relay:` stay dial/route
- [x] Drop communicating-identity fallbacks (direct target, group roster, directory shadow by Account)

## m4a — Shared mailbox without sibling starve (**M013**)

One Account → one `relay:` inbox (M006). Client cursors are already per-profile; **server ack-as-delete** was the starve point.

- [x] Messenger: soft-ack (validate cursor; do **not** `deleteMany`) — rely on **90d TTL** + soft per-mailbox FIFO cap (~1000–1200) + optional `clear` for GC
- [x] Document: `clear` remains account-wide; poll + local `relay_inbox_cursor.json` stay per-device
- [x] Tests: two consumers / soft-ack leaves rows for sibling poll
- [x] Client `MockRelayClient::AckInbox` soft-ack aligned
- [ ] (Follow-up) Optional server per-device watermarks + min-cursor GC — not required for first link

## m4b — Link-device + sync policy (**M012**)

- [x] `pp-browser-link-device-v1` codec + unit tests
- [x] `DataKeyVault::CreateWithDek` (shared DEK, new vault wrap)
- [x] `LinkDeviceExchange` export/import: account keys + **account KEM** + DEK + public PSKs; keep local Peer ID; **no** private `e2e` PSKs (**M014** / **M015**)
- [x] UI: **Copy link payload…** on the old device (Me → Security, registered + unlocked). New install: first-secrets **identity fork** → PIN for this device → paste payload into an **empty vault** (`CreateWithDek`). No in-place Security join. Already a person → **Reset this profile** first.
- [x] Push-register under same `relay:` after import

## m3 — Directory `endpoints[]` (**M017**)

Brief first (or same window), then client. Stops last-write-wins of a single `peer_id`. Hard cut: no top-level `peer_id` / `multiaddrs` on directory responses.

- [x] Brief: persist `endpoints[]` (`peer_id`, `multiaddrs[]`, `updated_at`) per Account ID
- [x] Register/renew **upserts** this device’s row; does not delete siblings
- [x] `GET /v1/users/by-account/:account_id`, route lookup, and search hits return `endpoints[]` only
- [x] Client: publish this install’s Peer ID on register/renew; parse `endpoints[]` for dial (LAN / open session, else newest `updated_at`)
- [x] Promote shipped shape into [SERVICE_ENDPOINTS.md](../../docs/contracts/SERVICE_ENDPOINTS.md)
- [x] Me → Security: one-active-sender help next to **Copy link payload…** (**M016**)

## m4c — Paste contacts + public thread index (**M018**) — superseded, do not build

Superseded by **M023**: a linked device reads account state from the locker (phase **s5**). Kept for history:

- [ ] Bundle `contacts[]` (`ContactToJson`) + `public_threads[]` (catalog only, `e2e_public`)
- [ ] Import merge by primary Account ID; `FindOrCreateDirectThread` for public index; still no private PSKs / transcripts
- [ ] Paste UI shows Account ID before import
- [ ] Raise `kMaxLinkDeviceBundleBytes` if needed (drop `public_threads[]` before `contacts[]` if still over cap)
- [ ] Unit tests: contacts applied; private threads/PSKs still omitted

## m4d — Unlink phase 1 (**M019**)

After m3 so Brief can drop the Peer ID row.

- [ ] This-install forget (reset profile / drop identity) + push unregister
- [ ] Signed remove of this `peer_id` from `endpoints[]` when possible
- [ ] Copy must not claim stolen device loses public/group keys
- [ ] Optional: device list on Me → Security (Peer IDs from `endpoints[]`)

## Later (not scheduled here)

- Device-scoped public PSKs stay off `public_psks[]` (**M020**)
- Unlink phase 2: account KEM rotation + remaining-device refresh (**M019**)
- ~~Sibling public-PSK + chat-index *refresh*~~ — sessions are per device pair (M021); account state syncs by the M023 change log (**s4**)
- Private Secure “add this device” transfer UX (**M014**)
- Dual-writer `sender_instance_id` (**D074**)
- Optional private PSK opt-in sync
- QR primary / short-code fallback (**M012**)
- Device-attested signing (S2) if compromise model demands it
- Multi-relay UX beyond binding data model
- Call multi-ring across device Peer IDs
- Server min-watermark GC across registered `device_id`s
- Account ML-DSA / Account ID rotation (new person)

## d — Per-device sessions (M021)

Branch `kenneth/per-device-sessions` (on top of the group protocol work, PR #383). Clean wire cutover at d2 (pre-release): apps before and after it do not read each other.

### d1 — Device keys and the device list

- [x] Each device mints its own ML-KEM key (additive in `identity.enc`; from the seed on a node); never in a link bundle or backup
- [x] Account card v2: device entries `{peer_id, signing key, KEM key}`, signed by the account key, newest `issued_at` wins (no union merge); re-issued only when it changes; at most 5 devices
- [x] Link bundle carries the issuing device's card; the new device signs itself in (`OwnCards::Learn`; siblings issuing at once converge)
- [x] The new device tells its siblings its card: a control-lane notice to our own mailbox (M022), on a change and once a start

### d2 — Envelopes and sessions per device pair (wire cutover)

- [x] Envelope v2: `sender_device`, `recipient_device` (empty = account copy), signed by the sender device's key (sign v3); the sender's card rides every key_init (`sender_card`)
- [x] Session store keyed by (peer account, peer device, channel); numbering per device pair (1:1 and group copies)
- [x] Send: one copy per device on the peer's card; account copy when there is no card yet; groups one copy per member device
- [x] Receive: skip siblings' copies; verify against the card (a carried card only when newer); replay / seq / sync state per sender device (account copies on public chats per sending device too)
- [x] Calls: unchanged on the account-level session (one account copy; single-device accounts work; several devices — d5)
- [x] Link bundle carries the card and no sessions; backups keep account-level sessions only
- [x] Unit tests: two linked devices both send and both read (1:1, group, account copies), new device's card, old card refused
- [x] Lab `linked_devices_both_send` (against the M022 relay branch)
- [x] **M022:** one relay record per account with a copy per device (one signature); split delivery (a device with a live link gets its own envelope over it, the relay if that fails; the others share one relay record); account copies carry the sender's card; the sibling card exchange over the mesh dropped
- [x] **M022 relay:** a record to one's own account allowed (and its history); mailbox cap doubled — messenger #17 and www #210, deployed (www 2026-10-29)
- [ ] Sent messages to our own other devices (the own-account record's copies) — d3

### d3 — Siblings

- [x] Copies to our own siblings: a `sent_copy` control-lane notice to our own account per sent public direct / group message (texts, attachments, cards); a sibling files it as ours (`RelayReceivePipeline::FileSentCopy`), opening the chat if needed; M022 §2 amended. gtest `sent_copy_test`; lab `linked_devices_see_what_each_sent`
- [x] Reactions and deletes on siblings: annotations are copied and applied as a peer's are (`FileSentCopy`: the annotation cap, a delete arriving first tombstones the message, a retract deletes it)
- [x] Attachments' bytes fetched on a sibling like a received one (`RelayReceiveOutcome::sibling_copy`: no inbox chrome, but the download is queued) — build-checked only: the probe and lab have no attachment path
- [x] Missing copy: ask the sender's device to send one (M021 §9) — `copy_ask` on the control lane after a 20 s wait (a copy over a link is not missing), answered only by the sending device, to a device on the asker's card (`MeshDeliveryOrchestrator::NoteMissingCopy` / `AnswerCopyAsk`, direct and group); gtest `copy_ask_test`; lab `a_device_asks_for_a_copy_it_missed` (the main device off, so nobody told Bob of the laptop)
- [x] A new device is introduced to the account's contacts: a restored one announces itself (`AnnounceRestoredDevice`); for a linked one the **master** does it when it takes in the card that adds it (`OwnCards::SetOnDeviceJoined` → `AnnounceOwnDevicesToContacts`) — the master has the contacts, the new device may not yet (its first locker sync may find none). Lab `linked_device_introduces_itself` (Bob's message reaches the laptop before the laptop wrote to anyone)

### d4 — Master device

Built as a **policy** (Hongwei, 2026-10-30): the rules below are followed by the account's own devices; every device still holds the account key (relay API auth, group-owner and payment signatures use it), so a stolen device could sign itself back in. Taking the key off non-master devices is a separate design (relay auth by device + card; the seed off the link bundle).

- [x] Master = the first device on the account card; only it changes the list (`OwnCards`); others re-issue only their own entry / the nickname; newest card wins (the interim union is gone)
- [x] Link: only the master gives out a transfer code (its consent); the new device appends itself once (`card_intent` join); a join reaching the master after its next card is kept while the code is out (`ExpectJoin`, 15 min)
- [x] Revoke = a list without the device (`RemoveDevice`); hand-over = another device first (`MakeMaster`); siblings hear through the shared mailbox, contacts by the `device_list` notice
- [x] Signed out: a device the newest list leaves out issues no card, refuses to send (`kSignedOutMessage`), stops syncing the locker
- [x] Restore = a new master with a fresh list (`card_intent` fresh, issued above the account's card floor); the old devices that hear it are signed out
- [x] Me → Security → Your devices (list, main device, Remove… / Make main device… on the master); probe `devices` / `remove_device` / `make_main`; gtest `own_cards_test`; lab `main_device_signs_a_device_out`, `restore_signs_out_the_other_devices`
- [ ] Only the master holds the account signing key (cryptographic revoke) — design first

### d5 — Calls on every device

- [x] Ring every device (the invite is an account copy); the first answer wins; the others stop — `call_answered_elsewhere` to our own devices on accept / decline, a sibling still ringing ends it locally with no history (`CallSessionWorkflow::StopRingingAnsweredElsewhere`); gtests (codec, no summary); lab `a_call_rings_every_device_and_stops_once_answered` ([CALLS.md](../../docs/architecture/CALLS.md))

### d6 — Later

- [ ] Directory carries the card (first contact without the account KEM)
- [ ] Card version in every envelope (peers fetch a newer list: faster revoke)
- [ ] Re-decide E027 device-lock on per-device sessions

## s — Account state (M023)

Decision: [M023](DECISIONS.md#m023--account-state-a-recovery-phrase-is-the-root-an-encrypted-account-locker-any-provider-can-hold). Nothing here is built. Today restore is unsupported ([at-rest A011](../at-rest-crypto/DECISIONS.md#a011--local-account-backup-and-restore-unsupported-in-the-product)); the lab drives the old `LocalAccountBackup` path only to keep that code honest until s5 replaces it.

### s1 — Recovery phrase

- [x] 256-bit seed ↔ 24 BIP-0039 English words with checksum (`foundation/crypto/RecoveryPhrase.*`, vectors from the BIP); account ML-DSA / ML-KEM seeds and locker key / signing seed by HKDF labels (`RecoverySeed.*`); locker id = BLAKE2b-256(locker signing pk)
- [x] New accounts mint from a recovery seed kept in `identity.enc` (`IdentityStore::RecoverySeed`); a link bundle carries it, refused unless it derives the bundle's keys (`LinkDeviceCodec`)
- [x] Existing accounts (random keys): **new accounts only** — they show no phrase (pre-release; COMPATIBILITY allows a wipe). Recorded in M023 *Open*
- [ ] Recovery tiers (M023 §3): **easy** (default, "Just continue") keeps the seed in the synced keychain — iOS `kSecAttrSynchronizable` (today's `OsSecureBlob_Ios.mm` is device-only: add a synced item), Android Block Store; **private** shows the phrase to write down; the easy tier's caveat in plain words
- [x] Me → Security → **Show recovery phrase…**: the PIN must open the vault again (the app default PIN: not asked, as for everything on that tier), shown for two minutes then hidden; en + zh-Hans (`SettingsSecurityActions`, `ConversationsFacade::RecoveryPhrase`); lab `recovery_phrase_needs_the_pin` (wrong PIN refused, stable words, a linked device shows the same)
- [ ] Tier switch: "Remove from iCloud / Google" (private, only after the phrase was shown); private → easy in one tap — needs the synced keychain first
- [ ] Desktop (no synced keychain): the phrase, or link from a phone
- [x] Unit tests: BIP vectors, typed / mistyped phrases, phrase → the same Account ID, labels never collide, link bundle seed check

### s2 — Locker format

- [x] Snapshot codec (`lib/people/AccountLocker.*`): state (relay binding, nickname, contacts, groups with the signed statement, card floors, providers) → JSON, length-prefixed and zero-padded to 4 KiB × 4ⁿ buckets, XChaCha20-Poly1305 under the locker key (AAD binds locker id + version), signed with the locker ML-DSA key over tag / id / version / sealed; locker id = BLAKE2b-256(signing pk) so `VerifyOwner` needs no account knowledge
- [x] Reader: `PickNewest` — the highest version that verifies and opens, none below the highest this device has seen
- [ ] Person-level preferences in the state (which ones follow the person: decide with s4)
- [x] Persist the highest version seen, per locker (`account_locker.json` in the profile dir; a provider serving an older copy of one locker says nothing about another)
- [x] Wire / disk doc: [ACCOUNT_LOCKER.md](../../docs/contracts/ACCOUNT_LOCKER.md) (keys, snapshot, state JSON, sync, restore, `account_locker.json`); HTTP in SERVICE_ENDPOINTS

### s3 — Providers

- [x] Relay locker API (web2 www `feat/account-locker`, local — **merges and deploys before the client**): `POST /v1/lockers/put` (relay half: the user's account signature; locker half: key hashes to the id, signature verifies; only newer; 4 lockers per user; ≤ 1 MiB) / `GET /v1/lockers/:id` by id alone; jest + golden vectors shared with the client ([SERVICE_ENDPOINTS § HTTP account lockers](../../docs/contracts/SERVICE_ENDPOINTS.md))
- [x] Client: `HttpLockerClient` (`lib/net`), `AccountLockerWorkflow` (`feature/conversations`: current state → seal → put; seed → get → verify → open); probe `locker_publish` / `locker_fetch`; lab `account_locker_round_trip`
- [x] When to publish: 5 s after a burst of contact / group / card changes, and at every start (`ConversationsHub::PublishAccountLockerSoon`) — only with a recovery seed and a relay id
- ~~pp-node provider~~ — dropped (M023 §6 amendment: nodes are stateless, caches only)
- [x] Exported file provider (core): `AccountLockerWorkflow::ExportFileJson` / `Fetch(seed, saved_copies)`, `ConversationsFacade::ExportAccountCopy`, restore with saved copies; probe `locker_export {path}` / `--restore-saved-copy`; gtest `account_locker_workflow_test`; lab `restore_from_a_saved_copy` (the file newer than the relay's copy wins)
- [x] Saved copy UI: Me → Security "Save a copy of my account…" (`SaveFileAs`: SDL save dialog on desktop / Android create-document, Files export picker on iOS) and "Use a saved copy…" on the phrase restore page (`ReadPickedFile`); gtest `pin_gate_controller_test` (the copy rides with the words); restore page checked on screen (Xvfb)
- [ ] Check the save / pick on devices: iOS export picker (`NativeFileDialog_Ios.mm`, not compiled here), Android content:// write and read
- [ ] Provider list in the state; the configured relay tried first

### s4 — Change log

- [x] Interim: each publish first merges what the locker holds (`MergeForPublish`: contacts union by Account ID, groups by the higher roster epoch, card floors max, providers union), so linked devices do not overwrite each other's additions. Removals do not carry yet
- [x] Contacts sync through the locker (the change log's medium, decided over a separate sibling message: one channel, offline-safe): a per-contact register (`ContactClock`: present / the user's fields' digest / when; a removal is a tombstone) — `Publish` is a two-way sync: note local edits, take newer remote registers into the address book, store the merged state, write nothing when nothing is new; the registers ride in the locker (`contact_clock`) and in `account_locker.json`
- [x] Groups need no entries: siblings are members as the same account and receive roster events; rosters follow the owner's signed statement (G012). Card floors: each device learns its own
- [x] Sync 5 s after local changes, at each start and every 10 min (`kLockerSyncIntervalMs`); lab `linked_devices_share_contact_edits` (a linked laptop's first sync brings the contacts; a remark and a removal on the laptop reach the phone; the removal is not undone)
- [x] A nudge to siblings after a write: the writer sends the `device_list` control-lane notice to its own mailbox (only with more than one device on the card); a verified sibling notice makes the receiver sync 5 s later; a sync with nothing new writes nothing, so it stops there. Lab `linked_devices_share_contact_edits` (the phone gets the laptop's remark without asking)
- [x] Clocks: a register moves past any time it has seen (`max(now, at + 1)`), so an edit made after seeing another always wins; only edits made concurrently on two devices are ordered by their wall clocks (stated in M023 and ACCOUNT_LOCKER § Sync)
- [x] Leaving a group: a group the account is out of (left, or removed — found by the member check) goes into the state as a tombstone (`left`, at its roster epoch); the merge prefers it at the same epoch, a re-invite's higher epoch beats it, restore skips it. Lab `group_removed_member_restores` (a second restore does not bring the group back)

### s5 — Restore and link

- [x] Restore from a typed phrase (`ConversationsHub::RestoreFromRecoveryPhrase`): phrase → locker (it holds the relay binding: no locker, no restore) → the link bundle the phrase reconstructs (`RecoveryBundle`, a fresh DEK) imported → contacts → groups / card floors / announce through the restore path once messaging is up; probe `--restore-phrase-file`, lab `restore_from_phrase` (every device gone, the group back and active, messages both ways, the locker republished)
- [x] Fresh-install UI: **Restore with my recovery phrase** on the identity fork → the link flow's PIN choice (Set a PIN / Just continue) → the 24-word page → `ProfileUnlockGate::CompletePhraseRestore` (shares `CompleteAccountImport` with the link) → unlocked, no first-run wizard; gtests for the gate and `PinGateController`; checked on screen (Xvfb)
- [ ] A new device signing a fresh list as master (d4)
- [x] Link: the new device reads the locker (the bundle carries the seed, no contacts); its first sync, at start, brings the contacts — lab `linked_devices_share_contact_edits`
- [ ] Restore finds the seed in the synced keychain first (easy tier), else asks for the phrase
- [x] Retire `LocalAccountBackup` (gone with its blob codec, the gate's "Restore my account", the probe's `--backup-store` / `--restore-backup`); the store interface lives on as `lib/people/SecureBlobStore` (easy tier), the surviving group / floor types as `AccountStateCodec`; the lab's restore scenarios (`restored_device_*`, `group_restore_checks_members`, `group_removed_member_restores`, `group_restored_owner_takes_newer_roster`) restore from the phrase. Only the retired item's cleanup goes at 1.1.0 (`kRetiredKeychainBackupRemoveAt`)

### s6 — Proof and promotion

- [ ] Lab: lose every device, restore from the phrase (and, with a file standing in for the synced keychain, the easy tier), contacts / groups / floors back, contacts' next messages arrive; a provider serving an old snapshot is overruled by a newer one
- [x] Promote the locker format and API into `docs/contracts/` (ACCOUNT_LOCKER, SERVICE_ENDPOINTS; AT_REST / DATA_LAYOUT point to them)
- [ ] Mark M023 implemented (after the tiers and the remaining s3 / s5 items)

