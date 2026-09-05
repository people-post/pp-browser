# Content CAS — decisions

Locked **2026-09-05**. Design: [DESIGN.md](DESIGN.md).

---

## C001 — Public confidentiality = clear bytes (v1)

**Status:** Accepted  

**Decision:** v1 public realm stores and serves **plaintext published bytes**. Privacy is unguessable `content_id` + optional unlisting. Link-scoped encryption (“anyone with link key”) is deferred.

**Rationale:** Matches true public/IPFS-like share; keeps v1 simple. Profile icons are already public plaintext (R003).

---

## C002 — Public `content_id` = hash(published bytes); publish = new object

**Status:** Accepted  

**Decision:** Public object id is the hash of the **published payload**. Publish always inserts a **new** index row with `published_from` → private id, even when published bytes equal private plaintext. Never advertise a private plaintext hash as a public provider key.

**Rationale:** Separates confidentiality domains and provenance; avoids accidental private-id leakage into discover/provide.

---

## C003 — Private object id = plaintext content hash (R016)

**Status:** Accepted  

**Decision:** Private CAS ids remain BLAKE2b-256 of **plaintext** (align [R016](../relay-blob-upload/DECISIONS.md#r016--content-addressed-local-blob-paths-d075) / D075). DEK/PPBA wrap is storage encoding, not identity.

**Rationale:** Preserves attachment dedupe/forwards and existing message references.

---

## C004 — Separate directory trees per realm

**Status:** Accepted  

**Decision:** Use `cas/private/blocks/` and `cas/public/blocks/` (not a single tree with only a DB flag).

**Rationale:** Safer backups, wipe, quota UX, and “clear downloaded attachments” without scanning mixed files.

---

## C005 — No cross-realm block sharing (v1)

**Status:** Accepted  

**Decision:** Private wrapped blocks and public clear blocks do not share files on disk. Two index rows if the same user bytes are both attached and published.

**Rationale:** Prevents public paths from pointing at DEK-wrapped private storage.

---

## C006 — Publish UX / revoke policy

**Status:** Accepted  

**Decision:**

- Publish only via explicit confirm-leading **Share publicly…** (not from chat decrypt).
- v1: user may publish attachments they can open locally (policy may tighten later).
- Revoke = unpin + stop provide; bytes may remain until GC unless user wipes.
- Quotas: track private vs public separately in Storage settings (limits TBD at implement).

**Rationale:** Clear product invariant; revoke is availability, not cryptographic erase.

---

## C007 — Attachment cutover = big bang

**Status:** Accepted  

**Decision:** When private CAS ships, **stop writing** durable attachment bytes under `threads/{id}/blobs/` as the source of truth. Migrate existing thread blobs into `cas/private` in the same cutover; thread dirs keep refs and session views only (`blobs_view/` may remain as materialized plaintext cache).

**Rejected:** Dual-write period (simpler ops story vs longer inconsistency window). Product not released — wipe/migrate local profiles is acceptable ([COMPATIBILITY](../../docs/contracts/COMPATIBILITY.md)).

**Rationale:** One durable location; avoids split-brain fetch ladder. Accept profile wipe/migrate cost pre-release.

---

## C008 — Wire `visibility` / realm

**Status:** Accepted  

**Decision:** Document realm/visibility in this project at P0. Add blob meta/wire fields when public fetch/provide ships (P3/P4). Product not released — no dual advertise required.

**Rationale:** Avoid premature wire churn before public share UX exists.

---

## C009 — CDN is a delivery path for public objects

**Status:** Accepted  

**Decision:** v1 focus = local CAS + peer blob provide/fetch. Optional upload/fetch via existing relay blob HTTP for public objects is a **path**, not a second store (same doctrine as private ciphertext CDN).

**Rationale:** Matches L4 “delivery orthogonal to conversation” and R002 (relay not archive of record).

---

## C010 — Two-level hex block sharding

**Status:** Accepted  

**Decision:** On-disk block paths are `cas/{realm}/blocks/{aa}/{bb}/{content_id_hex}` where `aa` / `bb` are the first two hex byte pairs of the 64-char content id (65 536 leaf directories). Same layout for private and public.

**Rationale:** Flat `blocks/` directories blow up under large object counts (pp-node / library scale). Two fixed hex levels are enough fan-out without deep nesting or content-dependent directory schemes.

---

## Amends

- Amends attachment layout expectations in [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) (planned CAS section).  
- Does not change L4 kinds; compositions for share/broadcast recorded in [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md).  
- Supersedes durable-byte role of per-thread `blobs/` once P2 cutover lands (R016 paths become views/refs).
