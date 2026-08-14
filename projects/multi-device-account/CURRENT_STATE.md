# Multi-device account — current state

**As of:** 2026-08-13 (**m3** `endpoints[]` hard cut + **M016** one-sender help landed)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + **account ML-KEM-768** + Account ID in `identity.enc` (schema v2) |
| Signing | Account ML-DSA-65 for register/API/envelopes |
| Brief (www) | Account KEM **1184**; by-account lookup; Account-first search; **`endpoints[]`** upserted on register (no top-level `peer_id`) |
| Directory (client) | `LookupByAccount`; hits/contact primary = Account; parse **`endpoints[]`**; dial prefers newest `updated_at` |
| Wire / threads / **calls** | Person = **Account ID**; route = **`relay:`** / Peer ID |
| Inbox | Soft-ack (**M013**); 90d TTL + FIFO cap; local cursor per profile |
| Vault | `CreateWithDek` / `ReplaceWithDek` for link import; per-profile `vault.bin` |
| Link-device | `pp-browser-link-device-v1`; copies account ML-DSA + **account KEM** + public PSKs; keeps Peer ID; Me → Security **copy payload** + one-sender help; new device **identity fork** + PIN + paste (empty vault); no private `e2e` PSKs (**M014** / **M015**); push re-attach after import. **No contacts / thread index yet** (**M018**) |
| Private PSK | Per device (**M005** / **M014**); one Secure session per pair; not device-keyed |
| Send | **D015** still one active sender; two linked senders can clash seq (**M016**) |

## Gap summary

| Gap | Phase |
|-----|--------|
| Contacts + public thread index in paste | **m4c** (**M018**) — next |
| Unlink / revoke | **m4d** phase 1; KEM rotation later (**M019**) |
| Sibling public-PSK + chat-index *refresh* | Later (**M015**) — not a second paste |
| Dual-writer seq | **D074** later |
| `e2e_public` send | e2e c3+ / **D100** (not this project) |

## Next

1. **m4c** paste `contacts[]` + `public_threads[]` (**M018**).
2. Dogfood two installs, **one sender**.
3. **m4d** unlink phase 1 (**M019**).
