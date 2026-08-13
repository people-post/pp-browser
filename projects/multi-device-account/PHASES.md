# Multi-device account — phases

Ordering only. Spec: [DESIGN.md](DESIGN.md). ADRs: [DECISIONS.md](DECISIONS.md).

**Rollout note (2026-08-13):** Skip interim dogfood/wipe between Account-ID cut and multi-device; keep implementing until a coherent multi-device slice is ready to test once.

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

## m4b — Link-device + sync policy (**M012**) — **next**

- [ ] `pp-browser-link-device-v1` codec + unit tests (see DESIGN § Link-device bundle)
- [ ] Export on old device (unlocked): seal Account ID + ML-DSA sk + DEK (+ public/group PSKs)
- [ ] Import on new device: own Peer ID, wrap DEK into new `vault.bin`, push-register under same `relay:`
- [ ] Enforce **no** auto-sync of private (`e2e`) PSKs
- [ ] Unlink device / revoke sketch (may defer details)

## m3 — Brief multi-device attach polish (after second Peer ID exists)

- [ ] Directory: list device Peer ID endpoints / multiaddrs (`endpoints[]` when needed)
- [ ] Dial preference / multi-device register-push UX polish

## Later (not scheduled here)

- Optional private PSK opt-in sync
- Device-attested signing (S2) if compromise model demands it
- Multi-relay UX beyond binding data model
- Call multi-ring across device Peer IDs
- Server min-watermark GC across registered `device_id`s
