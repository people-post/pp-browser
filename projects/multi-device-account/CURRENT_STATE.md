# Multi-device account — current state

**As of:** 2026-08-13 (m2a Brief directory landed; wire still `relay:`)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | `IdentityStore::SignBytes` / envelope verify / Brief register+API auth = **ML-DSA-65 only** |
| Brief (www) | KEM **1184**; `signature_alg=ml-dsa-65`; `account_id` required on `relay_users` |
| Directory | **`GET /v1/users/by-account/:account_id`**; search `q=` matches nickname / `relay:` / Account ID; hits **`account` primary**; route lookup returns `account_id`; register finish echoes `account_id` |
| Wire / threads | `ChatTargetKey` / `sender_contact_id` still `relay_user` + `relay:…` (**m2b**) |
| Vault | Per-profile `vault.bin`; no link-device / shared-DEK path yet |
| Private PSK | OOB per install; no multi-device sync |

## Gap summary

| Gap | Phase |
|-----|--------|
| Account ID on wire + catalog | m2b |
| Multi-device directory attach polish | m3 |
| Link-device, DEK seal, inbox cursors | m4 |

## Next

**m2b** client: hard cut `ChatTargetKey` / envelopes to Account ID (M009/M010).
