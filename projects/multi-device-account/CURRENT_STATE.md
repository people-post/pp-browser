# Multi-device account — current state

**As of:** 2026-08-13 (ADRs M009–M012 + D100 recorded; PQ signing hard cut; wire still `relay:`)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | `IdentityStore::SignBytes` / envelope verify / Brief register+API auth = **ML-DSA-65 only** |
| Brief (www) | KEM **1184**; `signature_alg=ml-dsa-65` required; `account_id` required on `relay_users` — wipe legacy Ed25519 rows |
| Directory | Search/lookup still relay-primary; **no** `by-account` yet (**m2a** / M011) |
| Wire / threads | `ChatTargetKey` / `sender_contact_id` still `relay_user` + `relay:…` (**m2b**) |
| Vault | Per-profile `vault.bin`; no link-device / shared-DEK path yet |
| Private PSK | OOB per install; no multi-device sync |

## Gap summary

| Gap | Phase |
|-----|--------|
| Brief by-account + Account-first search (`q=` Account ID / nickname / `relay:`) | m2a |
| Account ID on wire + catalog | m2b |
| Multi-device directory attach polish | m3 |
| Link-device, DEK seal, inbox cursors | m4 |

## Next

1. **m2a** www: `GET /v1/users/by-account/:account_id` + search `q=` / hit shape (M011).  
2. **m2b** client: hard cut `ChatTargetKey` / envelopes to Account ID (M009/M010).
