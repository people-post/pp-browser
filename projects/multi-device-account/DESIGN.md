# Multi-device account — design

**Maturity:** Design freeze for identity/key boundary (m0). Pre-release: **destructive** wire/storage changes allowed.  
**Canonical** for account vs device ids/keys. Related projects hold thin amend ADRs only — do not fork this matrix.

## Problem

Today one Ed25519 keypair is Peer ID + register proof + envelope signer (D096 “one keypair”). That blocks natural multi-device: two installs sharing Peer ID conflict on the mesh; cloning `identity.enc` races inbox cursors and `sender_seq` (D015).

## Target layering

```text
ACCOUNT (shared across linked devices)     DEVICE (per install)
─────────────────────────────────────      ──────────────────────────────
Account ID                                 Peer ID (from device keypair)
Account Ed25519 key → signs envelopes      Device Ed25519 key → dial/Noise
DEK (master secrets key)                   vault.bin = PIN wrap of same DEK
Public (/ later group) chat PSKs (sync)    device_id, push token
relay binding(s) per server                inbox cursor / ack watermark
Private (e2e) PSKs — only where set up     sender_instance_id
                                           multiaddrs / listen
```

**Slogan (replaces D096 single-device slogan):**  
**Account ID = who (person). Peer ID = which install (endpoint). Relay = route on a server. CAIP-10 = find/attest.**

## Identity roles

| Role | Value | Scope | First-class for |
|------|--------|-------|-----------------|
| **Account (person)** | `account:<base64url-unpadded(32-byte Ed25519 account pubkey)>` | Shared | Wire communicating identity (hard cut), link-device, DEK realm, directory person |
| **Endpoint (install)** | libp2p **Peer ID** (device keypair) | Per device | Dial/bind, mesh, call media peer |
| **Route** | `relay:<opaque_id>` | Per relay server, shared on devices using that server | Inbox, Brief register binding |
| **Find** | CAIP-10 (optional) | Alias | Search / attestation → Account ID (not wire) |

### Account ID format (frozen)

```text
account:<base64url-unpadded(Ed25519_account_public_key_32_bytes)>
```

- Ed25519 pubkeys are already 32 bytes (no further “compression”).
- Base64url, **no padding** — URL-safe, ~43 chars after prefix.
- Wire/storage: UTF-8 exact bytes, case-sensitive, no trim (same discipline as D082).

### Brief register binding

On a given Brief (or compatible) server:

1. Client proves control of the **account** private key (challenge/response).
2. Server accepts **at most one** active `relay_user_id` per Account ID.
3. Binding stored: `Account ID ↔ relay_user_id` (+ directory keys).
4. Other servers may later bind the **same** Account ID to a **different** `relay:` (multi-relay portability).

Devices do **not** each create a new person via register; they attach under the account (push `device_id`, Peer ID endpoints).

## Keys and signing

| Key | Holds | Signs / encrypts |
|-----|--------|------------------|
| **Account Ed25519** | All linked devices (under DEK) | **All** relay envelopes (S1) |
| **Device Ed25519** | One install | libp2p identity / Noise; **not** envelope `signature` in this freeze |
| **DEK** | Logical shared; each device wraps in own `vault.bin` | `identity` account material, syncable PSKs at rest |
| **Chat PSK** | Per `ChatTargetKey` | Message body AEAD (unchanged stack) |

**Envelope verify:** friends resolve **account** signing key for `sender_contact_id` = Account ID (directory / cache).  
**Install attribution:** `sender_instance_id` (D074) when multi-writer ships — not a second envelope signer in m0.

## Chat secret sync policy

| Tier | Auto-sync PSK to linked devices? |
|------|----------------------------------|
| **Private (`e2e`)** | **No** — only devices that established/imported that PSK |
| **Public (`e2e_public`)** | **Yes** (with account/DEK sync) when public tier + link ship |
| **Group** | **Yes** (direction; with group project) |

Private on a new device: OOB/import or explicit user opt-in copy — default is no fan-out.

## Wire / storage direction (hard cut, pre-release)

Target (implementation phases m1–m2):

- `ChatTargetKey` / `sender_contact_id` / AAD use **Account ID** (`peer_identity_kind` TBD: e.g. `account`), not `relay_user` as the person key.
- `relay:` remains route + Brief binding; inbox delivery uses the binding.
- Local `thread_id` stays device-local.
- Destructive vs today’s `relay:`-keyed threads: allowed pre-release (COMPATIBILITY wipe OK in dev).

Exact `ContactIdKind` / envelope field names: implement in m1–m2; record in DECISIONS when coded.

## Link-device (substance only)

1. Existing device unlocked (account key + DEK in memory).
2. New device: “I already have an account” → QR/short code.
3. Seal to new device: Account ID, account key material, DEK, public(/group) PSK material as policy allows — **not** private PSKs by default.
4. New device creates **its** `vault.bin` wrapping the **same** DEK; generates **device** keypair → Peer ID; registers push under account/`relay:` binding.

## Non-goals (m0)

- Nickname / UX chrome copy
- Device-signed envelopes (S2/S3) — upgrade path later if needed
- Cloud transcript backup as product
- Merging cross-relay histories automatically

## Cross-project pointers

| Topic | Home |
|-------|------|
| This model | **This DESIGN** |
| Amends D096 | [chat-storage D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device) |
| Account sign + private PSK policy | [e2e E025](../e2e-message-crypto/DECISIONS.md#e025--account-envelope-signing--private-psk-not-auto-synced) |
| Shared DEK / per-device vault | [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device) |
| Push device registry | [push-notifications](../push-notifications/) — remains per-install wake |
