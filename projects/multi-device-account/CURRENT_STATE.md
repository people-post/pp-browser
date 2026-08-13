# Multi-device account — current state

**As of:** 2026-08-13 (**m2b** done; **m4a** soft-ack **done**; **m4b** link-device next)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | Account ML-DSA-65 for register/API/envelopes |
| Brief (www) | KEM **1184**; by-account lookup; Account-first search |
| Directory (client) | `LookupByAccount`; hits/contact primary = Account; keys cached by Account |
| Wire / threads / **calls** | `ChatTargetKey` / envelopes / AAD / group roster / **call participants** = **Account ID** only; `sender_relay_id` / recipient / stream / inbox auth / dial PeerId = **route** (`relay:` or PeerId) |
| Inbox | One mailbox per `relay:`; **soft-ack** (M013) — no delete on ack; 90d TTL + soft FIFO cap (~1000–1200) + `clear`; local cursor per profile |
| Vault | Per-profile `vault.bin`; no link-device / shared-DEK path yet |
| Private PSK | OOB per install; no multi-device sync |

## Gap summary

| Gap | Phase |
|-----|--------|
| Soft-ack / shared mailbox (M013) | **m4a done** |
| Link-device, DEK seal | **m4b** (M012) — next |
| Multi-device directory `endpoints[]` | m3 (after second Peer ID) |

## Next

**m4b** link-device ritual (`pp-browser-link-device-v1` seal + vault wrap + push re-attach). Skip interim dogfood wipe until that slice is testable.
