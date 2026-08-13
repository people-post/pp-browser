# Multi-device account — current state

**As of:** 2026-08-13 (PQ hard cut: account ML-DSA on register/API/envelopes; wire identity still `relay:`)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | `IdentityStore::SignBytes` / envelope verify / Brief register+API auth = **ML-DSA-65 only** |
| Brief (www) | KEM **1184**; `signature_alg=ml-dsa-65` required; `account_id` required on `relay_users` — wipe legacy Ed25519 rows |
| Wire / threads | `ChatTargetKey` / `sender_contact_id` still `relay_user` + `relay:…` (**m2**) |
| Vault | Per-profile `vault.bin`; no link-device / shared-DEK path yet |
| Private PSK | OOB per install; no multi-device sync |

## Gap summary

| Gap | Phase |
|-----|--------|
| Account ID on wire + catalog | m2 |
| Directory polish / multi-device attach UX | m3 remainder |
| Link-device, DEK seal, inbox cursors | m4 |

## Next

**m2** hard cut `ChatTargetKey` / envelopes to Account ID.
