# Decisions — multi-device account

Format: **ID**, **date**, **decision**, **rationale**, **alternatives**.

Cross-project: [chat-storage D099](../chat-storage-and-memory/DECISIONS.md#d099--account-id-amends-d096-multi-device), [e2e E025](../e2e-message-crypto/DECISIONS.md#e025--account-envelope-signing--private-psk-not-auto-synced), [at-rest A010](../at-rest-crypto/DECISIONS.md#a010--shared-dek-per-device-vault-wrap-multi-device).

---

## M001 — Account ID as person root; Peer ID per device; relay as route

**Date:** 2026-08-11  
**Decision:** Introduce a portable **Account ID** as the account/person identity. **Peer ID** is **per device** (endpoint). **`relay_user_id`** is a **per-server route binding**, not the person root. **CAIP-10** remains find/attest only (D091).  
**Rationale:** Enables multi-relay under one person; avoids libp2p Peer ID conflicts when two installs are online; matches pre-release willingness for a hard cut.  
**Alternatives:** Account = `relay:` only (rejected — provider-bound); account = Peer ID (rejected — multi-device clash); account = CAIP-10 (rejected — optional, not wire).

---

## M002 — Account ID format: PQ hash-binding of ML-DSA-65 pubkey

**Date:** 2026-08-11  
**Updated:** 2026-08-11 — aggressive PQ: Account ID no longer Ed25519-derived.  
**Decision:**

```text
account:<base64url-unpadded(BLAKE2b-256(ML-DSA-65 account public key))>
```

Directory / identity publish the full **1952-byte ML-DSA-65** public key; Account ID **must** equal the hash binding. UTF-8 exact, case-sensitive, no trim.  
**Rationale:** PQ-rooted person id without putting ~2.6k chars on every wire field; forgery still requires ML-DSA secret key.  
**Alternatives:** Full ML-DSA pubkey in id (accepted if desired later); Ed25519-derived id (rejected for account root); random UUID (not crypto-bound).

---

## M003 — Account ML-DSA-65 signs envelopes; device Ed25519 is endpoint-only

**Date:** 2026-08-11  
**Updated:** 2026-08-11 — aggressive PQ: account signing is **ML-DSA-65 only** (not Ed25519 hybrid).  
**Decision:** **Account** key = **ML-DSA-65** (vendored `mldsa-native`) signs all relay envelopes. **Device** Ed25519 keypair derives Peer ID / libp2p only — not envelope `signature`. Installs distinguished by `sender_instance_id` when multi-writer ships (D074).  
**Rationale:** Maximize PQ for person-level auth; classical remains only where mesh forces it.  
**Alternatives:** Ed25519+ML-DSA hybrid (S1 classic); device signs + account attests (S2).

---

## M004 — Shared DEK; per-device vault wrap

**Date:** 2026-08-11  
**Decision:** Linked devices share one **DEK**. Each install has its own **`vault.bin`** (PIN-derived wrap; PIN may differ). Link seals DEK to the new device; new device wraps into its vault.  
**Rationale:** Minimizes shared on-disk vault material while keeping one secrets realm; matches at-rest A001 layering (PIN wraps DEK).  
**Alternatives:** Different DEK per device (heavier re-seal); clone identical `vault.bin` (same PIN everywhere).

---

## M005 — Private PSKs not auto-synced; public(/group) may sync

**Date:** 2026-08-11  
**Decision:** **Private (`e2e`) `master_psk` / retired ledger are not auto-synced** to linked devices. Public (`e2e_public`) and group pair keys **may** sync with account/DEK when those tiers + link ship. Body encryption remains PSK AEAD on all tiers — “device-bound private” means **which devices hold the PSK**, not a different cipher.  
**Rationale:** Preserves private-tier assurance under account signing (S1); stolen/linked laptop does not silently receive every private chat key.  
**Alternatives:** Sync all PSKs with DEK; device-signed private envelopes only (S3).

---

## M006 — Brief register binding: one relay id per Account ID per server

**Date:** 2026-08-11  
**Decision:** Registration proves the **account** key. Each Brief-compatible server maintains **at most one** active `relay_user_id` per Account ID. Other servers may bind the same Account ID to different route ids later.  
**Rationale:** Portable account with provider-local mailboxes; devices attach under the binding rather than re-registering as new people.  
**Alternatives:** Keep register tied to device/Peer ID key only; multiple relay ids per account on one server.

---

## M007 — Pre-release hard cut: communicating identity → Account ID

**Date:** 2026-08-11  
**Decision:** Target wire and `ChatTargetKey` use **Account ID** as communicating identity (destructive OK pre-release). `relay:` remains route/binding. Implementation timing in [PHASES.md](PHASES.md) m1–m2.  
**Rationale:** Avoid shipping `relay:`-as-person then migrating again.  
**Alternatives:** Soft cut (Account ID for link/DEK only; wire stays `relay:`) — faster but weak multi-relay threads.

---

## M008 — PQ libraries and KEM: mlkem-native + mldsa-native; ML-KEM-768 only

**Date:** 2026-08-11  
**Decision:** Vendor **mlkem-native v2.0.0** (ML-KEM-768, C backend) and **mldsa-native v2.0.0** (ML-DSA-65, C backend) under `third_party/`. Public auto-key uses **ML-KEM-768 only** (no X25519 hybrid). App wrappers: `HybridKem` (ML-KEM-768; name retained) and `MlDsa`. Symmetric stack remains libsodium; libp2p remains BoringSSL/Ed25519 for device Peer ID.  
**Rationale:** High-assurance PQCP implementations; matches aggressive PQ account model; Brief updates KEM blob size to 1184 in parallel.  
**Alternatives:** liboqs umbrella; keep X25519+Kyber-draft BoringSSL experimental path.
