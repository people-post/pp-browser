# Multi-device account — phases

Ordering only. Spec: [DESIGN.md](DESIGN.md). ADRs: [DECISIONS.md](DECISIONS.md).

**Rollout note (2026-08-13):** Skip interim dogfood/wipe between Account-ID cut and multi-device; keep implementing until a coherent multi-device slice is ready to test once.

**Next:** per-device sessions (**M021**), phases **d1–d6** below. m4c / m4d are folded into them (contacts in the link paste stays m4c; unlink becomes the d4 revoke).

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

## m4c — Paste contacts + public thread index (**M018**)

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
- Sibling public-PSK + chat-index *refresh* when both devices are reachable (**M015**)
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
- [ ] The new device tells its siblings its card — the relay refuses an account's envelopes to itself; over Amp card exchange (built, not yet passing the lab) or a relay change (open)

### d2 — Envelopes and sessions per device pair (wire cutover)

- [x] Envelope v2: `sender_device`, `recipient_device` (empty = account copy), signed by the sender device's key (sign v3); the sender's card rides every key_init (`sender_card`)
- [x] Session store keyed by (peer account, peer device, channel); numbering per device pair (1:1 and group copies)
- [x] Send: one copy per device on the peer's card; account copy when there is no card yet; groups one copy per member device
- [x] Receive: skip siblings' copies; verify against the card (a carried card only when newer); replay / seq / sync state per sender device (account copies on public chats per sending device too)
- [x] Calls: unchanged on the account-level session (one account copy; single-device accounts work; several devices — d5)
- [x] Link bundle carries the card and no sessions; backups keep account-level sessions only
- [x] Unit tests: two linked devices both send and both read (1:1, group, account copies), new device's card, old card refused
- [ ] Lab `linked_devices_both_send`: fails — the new device's first message does not reach the peer yet (under investigation)
- [ ] **M022:** one relay record per account with an entry per device (one signature); split delivery (mesh for linked devices, one relay record for the rest, a follow-up for a failed mesh send); a record to our own account for our other devices; replaces the sibling card exchange over the mesh
- [ ] **M022 relay (messenger):** a record to one's own account allowed (and its history); mailbox cap doubled — merges and deploys before the client

### d3 — Siblings

- [ ] Copies to our own siblings (sent messages show on every device)
- [ ] Missing copy: ask the sender's device to send one (M021 §9)
- [ ] A new device introduces itself to its contacts (opens its device pairs)

### d4 — Master device

- [ ] Only the master holds the account signing key; two-step link (new device shows its keys, master signs it in)
- [ ] Revoke = a list without the device; master hand-over while both are online
- [ ] Restore = a new master with a fresh list; an old master that sees it stops sending

### d5 — Calls on every device

- [ ] Ring every device; the first answer wins; the others stop

### d6 — Later

- [ ] Directory carries the card (first contact without the account KEM)
- [ ] Card version in every envelope (peers fetch a newer list: faster revoke)
- [ ] Re-decide E027 device-lock on per-device sessions

