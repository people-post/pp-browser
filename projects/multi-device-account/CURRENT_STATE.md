# Multi-device account — current state

**As of:** 2026-08-13 (**m2a** + **m2b** landed; m4 link-device next)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | Account ML-DSA-65 for register/API/envelopes |
| Brief (www) | KEM **1184**; by-account lookup; Account-first search |
| Directory (client) | `LookupByAccount`; hits/contact primary = Account; keys cached by Account |
| Wire / threads | `ChatTargetKey` / `sender_contact_id` / AAD / group roster = **Account ID** only (no communicating-identity fallback to `relay:`); `sender_relay_id` / recipient / stream / inbox auth = **`relay:`** route |
| Vault | Per-profile `vault.bin`; no link-device / shared-DEK path yet |
| Private PSK | OOB per install; no multi-device sync |

## Gap summary

| Gap | Phase |
|-----|--------|
| Multi-device directory attach polish | m3 |
| Link-device, DEK seal, inbox cursors | m4 |

## Next

**m4** link-device (M012), or **m3** multi-device attach polish if needed first.
