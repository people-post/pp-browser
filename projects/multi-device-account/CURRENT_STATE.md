# Multi-device account — current state

**As of:** 2026-08-11 (m0 design + PQ libs vendored; envelope hard cut not wired)

## Code today (single-device assumptions)

| Area | Behavior |
|------|----------|
| `LocalIdentity` | One Ed25519 + KEM keypair; `peer_id` derived in memory; `relay_user_id` after register |
| KEM | **ML-KEM-768** via `HybridKem` + `third_party/mlkem-native` (legacy 1216 hybrid wiped on size mismatch) |
| Account ML-DSA | Wrapper `MlDsa` + `third_party/mldsa-native` built/tested; **not** yet envelope/register signer |
| D096 / Me | Peer ID treated as product “who”; one keypair for Peer ID + register + envelope sign |
| Wire / threads | `ChatTargetKey` / `sender_contact_id` use `relay_user` + `relay:…` |
| Vault | Per-profile `vault.bin` wraps DEK; no link-device / shared-DEK path |
| Private PSK | OOB per install; no multi-device sync story |
| Push | `device_id` + token under `relay_user_id` (multi-token possible) |
| Inbox | One `relay_inbox_cursor.json` watermark; ack deletes — not multi-reader |
| Multi-send | D015 single active sender; D074 `sender_instance_id` reserved unused |

## Target (this project)

See [DESIGN.md](DESIGN.md). m0 decisions: [DECISIONS.md](DECISIONS.md) M001–M008.

## Gap summary

| Gap | Phase |
|-----|--------|
| Account vs device key material in identity/storage | m1 |
| Account ID on wire + catalog | m2 |
| Brief account-proof register + endpoint list | m3 |
| Link-device, DEK seal, PSK sync policy enforcement | m4 |

## Next

Implement only after m0 review; start at [PHASES.md](PHASES.md) **m1**.
