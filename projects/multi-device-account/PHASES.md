# Multi-device account — phases

Ordering only. Spec: [DESIGN.md](DESIGN.md). ADRs: [DECISIONS.md](DECISIONS.md).

## m0 — Design freeze

- [x] Account ID format (M002)
- [x] Account vs device keys; S1 signing (M003)
- [x] Shared DEK / per-device vault (M004)
- [x] Private PSK not auto-synced (M005)
- [x] Brief register binding (M006)
- [x] Hard-cut direction for wire identity (M007)
- [x] Thin amend ADRs in chat-storage / e2e / at-rest
- [x] Project README + CURRENT_STATE

## m1 — Types and storage split

- [x] Account keypair + Account ID in identity model (separate from device key / Peer ID)
- [x] Persist account material under DEK; device key local to install
- [x] Profile/AAD story compatible with shared DEK across devices
- [x] Unit tests for Account ID encode/decode (M002) — `ml_dsa_test` + identity_store persist
- [x] Client register + `IdentityStore::SignBytes` / envelope verify use account ML-DSA-65 (hard cut)

## m2 — Wire / thread hard cut

- [ ] `ChatTargetKey` / envelope `sender_contact_id` → Account ID
- [ ] Ingest verify against account signing key
- [ ] Migrate or wipe pre-cut `relay:`-keyed local state (pre-release OK)
- [ ] Promote normative snippets to `docs/contracts/` when behavior ships

## m3 — Brief binding + directory

- [ ] Register proves account key; one `relay:` per Account ID per server
- [ ] Directory: person = Account ID; list device Peer ID endpoints / multiaddrs
- [ ] Update `SERVICE_ENDPOINTS` contract when API stabilizes

## m4 — Link-device + sync policy

- [ ] Link ritual: seal account key + DEK (+ syncable PSKs) to new device
- [ ] New device: own `vault.bin`, own Peer ID, push register
- [ ] Enforce **no** auto-sync of private (`e2e`) PSKs
- [ ] Per-device inbox cursor (or equivalent) so ack does not starve siblings
- [ ] Unlink device / revoke sketch (may defer details)

## Later (not scheduled here)

- Optional private PSK opt-in sync
- Device-attested signing (S2) if compromise model demands it
- Multi-relay UX beyond binding data model
- Call multi-ring across device Peer IDs
