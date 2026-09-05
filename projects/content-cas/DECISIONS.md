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

**Follow-up:** After cutover, legacy thread `blobs/` read/write and the unlock migrate helper were removed. Pre-release profiles without CAS must be wiped/recreated.

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


## C011 — Private presentation: RAM LRU + video size gate

**Status:** Accepted  

**Decision:**

1. **Scope = private realm only.** Memory / session plaintext presentation policy applies to **private** objects (DEK-wrapped). Public realm stores clear bytes (C001); there is no equivalent privacy concern for keeping public plaintext off disk during view.
2. **Images and video posters:** Prefer an in-process **RAM LRU** of private plaintext (`AttachmentPlaintextMemoryCache`), keyed by content-hash hex. Admit small objects only (≤8 MiB/entry; 64 entries / 64 MiB budget). Fill on save/load. **Clear the LRU on ClearDek** together with wiping all thread `blobs_view/` trees (`WipeAllAttachmentViewCaches`).
3. **`blobs_view/` remains** a session on-disk plaintext materialization for RmlUi file `src` / OS open when needed. It is not durable truth (CAS is). Future work may replace file `src` with in-memory textures or stream decrypt without changing CAS.
4. **Private video inline gate:** Videos larger than Soft auto-download (`kMaxInlinePrivateVideoBytes` = 4 MiB) skip session `blobs_view` materialization and true frame extract until **explicit open**. Use soft poster placeholder when over the gate. **Gate presentation / playback, not CAS ingest** — large videos still save to private CAS.
5. **Non-blocking for later:** Size gate is an early workaround. On-demand chunk decrypt and alternate presenter backends remain allowed follow-ons.

**Rationale:** ClearDek must not leave private plaintext in RAM or `blobs_view`. Images benefit from RAM without forcing large video plaintext onto disk for every scroll. Public objects need no DEK wipe path for clear bytes.

**Rejected (for now):** Blocking CAS write of large videos; requiring full stream-decrypt before any video UI.

---

## C012 — Module home: stay in messaging until public CAS has a second owner

**Status:** Accepted  

**Decision:**

1. **Keep for now** under `src/domain/messaging/`: `CasStore`, `ObjectIndex`, `CasTypes`, `AttachmentCache`, `AttachmentPlaintextMemoryCache` (and related private-attachment I/O). Callers today are almost entirely conversations/attachments; domain peers must stay independent ([SRC_LAYOUT](../../docs/architecture/SRC_LAYOUT.md)).
2. **Do not** move this into `foundation/` — it is a product durable store + realm policy, not kernel path/config/crypto primitives.
3. **Planned peel:** introduce a new domain peer (preferred name **`domain/content`**) when **P3/P4** (public library / Share publicly… / provide-fetch) gives CAS a **second real owner** outside messaging. That peer should own:
   - `CasStore`, `ObjectIndex`, realm types
   - optional thin presentation helpers (RAM LRU / view wipe) if still realm-generic
4. **Leave behind in messaging / feature:** chat-specific policy — attachment download/suppression, chat blob responders, thread UX gates that call into content via `common` contracts + feature wiring.
5. **No premature empty peer** that messaging still monopolizes; extract when the second consumer compiles against content without linking messaging codecs/stores.

**Rationale:** Avoids cross-peer edges and churn during private cutover (P1–P2 / C011). Aligns peel with the moment public CAS stops being “messaging storage with a fancy path.”

**Rejected (for now):** Immediate `domain/content` (or `domain/cas`) split; placing CAS in foundation.

---
## Amends

- Amends attachment layout expectations in [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md) (planned CAS section).  
- Does not change L4 kinds; compositions for share/broadcast recorded in [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md).  
- Supersedes durable-byte role of per-thread `blobs/` once P2 cutover lands (R016 paths become views/refs).
- C011 amends attachment presentation expectations in [AT_REST_ENCRYPTION](../../docs/contracts/AT_REST_ENCRYPTION.md) and [DATA_LAYOUT](../../docs/contracts/DATA_LAYOUT.md).
- C012 records deferred `domain/content` peel; amends domain peer table intent in [SRC_LAYOUT](../../docs/architecture/SRC_LAYOUT.md).
