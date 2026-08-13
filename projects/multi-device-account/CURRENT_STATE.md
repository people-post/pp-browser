# Multi-device account — current state

**As of:** 2026-08-13 (**m4b** codec + exchange landed; UI / push re-attach next)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + Account ID in `identity.enc` (schema v2); KEM ML-KEM-768 |
| Signing | Account ML-DSA-65 for register/API/envelopes |
| Brief (www) | KEM **1184**; by-account lookup; Account-first search |
| Directory (client) | `LookupByAccount`; hits/contact primary = Account; keys cached by Account |
| Wire / threads / **calls** | Person = **Account ID**; route = **`relay:`** / Peer ID |
| Inbox | Soft-ack (**M013**); 90d TTL + FIFO cap; local cursor per profile |
| Vault | `CreateWithDek` for link import; per-profile `vault.bin` |
| Link-device | `pp-browser-link-device-v1` + `LinkDeviceExchange` (no UI yet); public PSKs only |
| Private PSK | Per device (**M005** / **M014**); one Secure session per pair; not device-keyed |

## Gap summary

| Gap | Phase |
|-----|--------|
| Link UI + push re-attach | **m4b** remaining |
| Multi-device directory `endpoints[]` | m3 |
| Dual-writer seq on one Secure lock | D074 later |

## Next

Me → export/import UI, then push-register the new Peer ID under the same `relay:`.
