# Multi-device account — current state

**As of:** 2026-08-13 (**m4b** paste landed; **M016–M019** accepted — next is **m3**)

## Code today

| Area | Behavior |
|------|----------|
| `LocalIdentity` | Device Ed25519 → Peer ID; account ML-DSA-65 + **account ML-KEM-768** + Account ID in `identity.enc` (schema v2) |
| Signing | Account ML-DSA-65 for register/API/envelopes |
| Brief (www) | Account KEM **1184**; by-account lookup; Account-first search; **one** `peer_id` (last-write-wins on register) |
| Directory (client) | `LookupByAccount`; hits/contact primary = Account; signing + KEM cached by Account |
| Wire / threads / **calls** | Person = **Account ID**; route = **`relay:`** / Peer ID |
| Inbox | Soft-ack (**M013**); 90d TTL + FIFO cap; local cursor per profile |
| Vault | `CreateWithDek` / `ReplaceWithDek` for link import; per-profile `vault.bin` |
| Link-device | `pp-browser-link-device-v1`; copies account ML-DSA + **account KEM** + public PSKs; keeps Peer ID; Me → Security **copy payload**; new device **identity fork** + PIN + paste (empty vault); no private `e2e` PSKs (**M014** / **M015**); push re-attach after import. **No contacts / thread index yet** (**M018**) |
| Private PSK | Per device (**M005** / **M014**); one Secure session per pair; not device-keyed |
| Send | **D015** still one active sender; two linked senders can clash seq (**M016**) |

## Gap summary

| Gap | Phase |
|-----|--------|
| Directory last-write-wins `peer_id` | **m3** (**M017**) — next |
| One-sender help copy | **m3** (**M016**) |
| Contacts + public thread index in paste | **m4c** (**M018**) |
| Unlink / revoke | **m4d** phase 1; KEM rotation later (**M019**) |
| Sibling public-PSK + chat-index *refresh* | Later (**M015**) — not a second paste |
| Dual-writer seq | **D074** later |
| `e2e_public` send | e2e c3+ / **D100** (not this project) |

## Next

1. **m3** Brief + client `endpoints[]` (**M017**) and Security one-sender help (**M016**).
2. **m4c** paste `contacts[]` + `public_threads[]` (**M018**).
3. Dogfood two installs, **one sender**.
4. **m4d** unlink phase 1 (**M019**).
