# Multi-device account — design

**Status:** Design freeze (m0) + contact/wire/directory ADRs (**M009–M011**). Implementation: **m1** keys done; Brief **M011** then client **m2**; **m4** after ([M012](DECISIONS.md#m012--link-device-ritual-deferred-until-m4)).  
**Related:** [e2e-message-crypto](../e2e-message-crypto/), [at-rest-crypto](../at-rest-crypto/), [chat-storage D096](../chat-storage-and-memory/DECISIONS.md#d096--multi-device-and-sync-amends-d092) / [D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device) / [D100](../chat-storage-and-memory/DECISIONS.md#d100--release-scope-b-pq-account-id), [docs/contracts/COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md).

## Problem

Today the product collapses **person**, **device**, and **Brief route** into one handle (`relay:` / Peer ID). That cannot support multiple devices under one person, or one person reachable via more than one relay, without breaking threads, E2E AAD, and contact identity.

## Goals

1. One **Account ID** for the person across devices and relays.
2. Many **device identities** (Peer ID / Ed25519) under that account.
3. **`relay:`** as a **route** (inbox / API auth), not the person key for chat state.
4. Shared **DEK** and public/group PSKs across linked devices; private `e2e` PSKs stay device-local (**M005**).
5. Pre-release **hard-cut** to Account ID on wire and catalog (**M007**) — no dual-id soft migration.

## Non-goals (this project)

- Full cloud message sync / CRDT history (**D096** still later).
- Replacing libp2p Peer ID for transport.
- Turning Brief into a full IdP beyond directory + route binding.

## Identity model

| Concept | Format / crypto | Role |
|---------|-----------------|------|
| **Account ID** | `account:` + base64url-unpadded(BLAKE2b-256(ML-DSA-65 pk)) | Communicating identity: contacts primary, threads, envelopes, AAD, signing-key cache |
| **Account signing key** | ML-DSA-65 | Register, Brief API auth, envelope signatures |
| **Device Peer ID** | libp2p / Ed25519 | Direct streams, dial, call media attach |
| **Relay user id** | `relay:` + … | Inbox, send route, device↔Brief binding (**M006**) |

**Invariant:** Account ID == hash(account ML-DSA public key). Changing the account signing key changes Account ID (new person).

```text
Account (Account ID + ML-DSA)
  ├── Device A (Peer ID_A, local DEK unwrap, local e2e PSKs)
  ├── Device B (Peer ID_B, …)
  └── Routes: relay:… @ Brief (and later more relays)
```

## Contact and wire (M009, M010)

- **`ContactIdKind::Account`** / `peer_identity_kind=account`; value = full Account ID string.
- Account is **primary** on the person; `relay_user` and `peer_id` are secondary.
- Envelopes: `sender_contact_id` + AAD + `ChatTargetKey` use Account ID.
- Relay HTTP auth / inbox requester stay **`relay:`**.

## Brief directory (M011) — shipped early (m2a)

| API | Role |
|-----|------|
| `GET /v1/search?q=` | Match **nickname**, **`relay:`**, and **Account ID** (incl. prefix). Hits: top-level `account_id`; `ids[]` with `account` **primary**. |
| `GET /v1/users/by-account/:account_id` | Person lookup (keys, `relay_user_id`, `signature_alg`, …). |
| `GET /v1/users/:relay_user_id` | Route lookup; response includes `account_id`. |
| `POST /v1/register/finish` | Echoes `account_id`. |

## Threat notes (short)

- Account ML-DSA compromise = full account forge → recovery/revoke later.
- Link-device is a high-value ceremony (**M012**): confirm on old device; fingerprint Account ID on new; never seal private `e2e` PSKs.
- Brief sees Account ID ↔ `relay:` binding and pubkeys; not DEK or message plaintext.

## Open after freeze

- Unlink / revoke UX beyond m4 sketch.
- Multi-relay `endpoints[]` richness (M011 allows single `peer_id` until then).
- Account key rotation (new Account ID) product story.

## Link-device bundle (m4b)

Transport: QR primary, short code fallback (**M012**). JSON format **`pp-browser-link-device-v1`**.

**Sealed to new device (never private `e2e` PSKs — M005 / M014):**

| Field | Notes |
|-------|--------|
| `account_id` | Full `account:…` (must match hash of `account_ml_dsa_pk_b64`) |
| `account_ml_dsa_pk_b64` / `account_ml_dsa_sk_b64` | Account ML-DSA-65 |
| `dek_b64` | Shared DEK — new device wraps into its own `vault.bin` (`CreateWithDek`) |
| `public_psks[]` | Optional; **`e2e_public` only** |
| `relay_user_id` | Existing Brief binding |
| `created_at` / `expires_at` | Default TTL 15 minutes |

**New device after import:** keep local device Ed25519 / Peer ID / KEM; replace account keys + `relay:`; choose PIN → wrap DEK. Push re-attach and UI still follow.

**Private Secure (M014):** one session per pair; transfer copies PSK+seq, not a device-keyed thread.
