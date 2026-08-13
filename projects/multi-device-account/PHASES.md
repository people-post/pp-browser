# Multi-device account — phases

Ordering only. Spec: [DESIGN.md](DESIGN.md). ADRs: [DECISIONS.md](DECISIONS.md).

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

## m3 — Brief multi-device attach polish

- [ ] Directory: list device Peer ID endpoints / multiaddrs (richer than single peer when needed)
- [ ] Multi-device register/push attach UX as needed

## m4 — Link-device + sync policy (M012)

- [ ] Link ritual: QR + short code; seal account key + DEK (+ syncable PSKs) to new device
- [ ] New device: own `vault.bin`, own Peer ID, push register
- [ ] Enforce **no** auto-sync of private (`e2e`) PSKs
- [ ] Per-device inbox cursor (or equivalent) so ack does not starve siblings
- [ ] Unlink device / revoke sketch (may defer details)

## Later (not scheduled here)

- Optional private PSK opt-in sync
- Device-attested signing (S2) if compromise model demands it
- Multi-relay UX beyond binding data model
- Call multi-ring across device Peer IDs
