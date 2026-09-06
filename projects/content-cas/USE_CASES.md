# Content CAS — use cases

**Status:** Working product sketch (2026-09-06)  
**Design:** [DESIGN.md](DESIGN.md) · **Decisions:** [DECISIONS.md](DECISIONS.md) (incl. **C013**)  
**Related:** [NAME_DIRECTORY_NORTH_STAR](../p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md) (phone book ≠ content catalog), [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md) (share = discover + blob + rpc)

Product intent for P3+ library / public share. Disk and realm invariants stay in DESIGN; this file is the UX/capability story.

## Mental model

| Axis | Meaning | Product language |
|------|---------|------------------|
| **Realm** | Confidentiality: DEK-wrapped private vs clear public | Private / Public |
| **Pin** | Retention intent: keep/host vs GC-able cache | **Kept** / **Cache** (advanced: Pin / Unpin) |
| **Serve** | Can others fetch bytes / browse a catalog from this endpoint? | Needs reachable Node / Home Node for open shelf |

**Library** = indexed CAS objects under this profile (`object_index.db` + realm trees). Not a general OS file manager.

**Share publicly…** = publish a **new** public object (copy/promote with `published_from`), not a move. Private may remain. (**C002 / C005**)

**Pin ≠ public.** Publishing pins the public row by default; pin alone never promotes private → public. (**C013**)

```text
directory (phone book):  name → PeerId, endpoints, caps
                         optional: content_library / blob_provide

on PeerId (or Home Node):  list kept-public catalog  →  [{ content_id, name, … }]
bytes:                     blob fetch by content_id (peer first; CDN assist optional)
```

The mesh **directory does not store per-file catalogs** (filenames / content ids as person-record fields). It may advertise capability (+ optional manifest tip). Full list is answered by the serving peer / Node. (**C013**)

## Share modes

| Mode | Stable serve required? | Discoverability | Notes |
|------|------------------------|-----------------|-------|
| **A. Link / tip** | No | Holders of `content_id` / tip only | One object; optional CDN assist while peer offline |
| **B. Contact push** | Weak | Thread recipients | Attach / send public object in chat |
| **C. Open library** | **Yes** | Resolve account → catalog rpc (ACL) | Capability on directory; list + provide on Node |
| **D. Hosted shelf** | No (service always-on) | URL / account shelf on storage | Separate durability product — not default relay blob |

Ship **A/B** before **C**. Treat **D** as an explicit later product if Home Node adoption is insufficient; do not silently make relay CDN an archive (**R002** / **C009**).

---

## Use cases

### U1 — Browse library

User opens Files / Library and sees CAS objects for this profile.

- Filters: Private / Public / Cache (unpinned) / All  
- Primary badge: realm; secondary: Kept vs Cache  
- Show name, size, mime, created; optional “from chat …” / `published_from`  
- Quotas: private vs public tracked separately (C006)  
- Locked vault: private rows hidden or locked until unlock  

### U2 — Open private from library

Same presentation rules as chat attachments while unlocked (C011). Never provides private bytes on the public mesh.

### U3 — Chat attach stays private

Sending / receiving chat attachments writes **private** CAS only. Decrypt never writes `cas/public/`.

### U4 — Share publicly…

From a private library row or attachment menu → confirm-leading **Share publicly…** → new public object appears as **Kept**; optional copy tip / announce (P4). Private row may show “Also shared publicly” via `published_from`. Publish pins the public row (C013); no separate “pin” step required.

### U5 — Unpublish / revoke

From a public **Kept** row → confirm → stop provide + unpin. Row may move to Cache or hide until GC. Revoke ≠ cryptographic erase unless user chooses wipe (C006).

### U6 — Receive / help cache (P4+)

Fetching someone else’s public `content_id` stores **public + unpinned** (Cache). **Clear public cache** must not delete **Kept** objects.

### U7 — Storage hygiene

Three distinct intents:

1. Clear downloaded private attachments / private cache policy  
2. Clear public **Cache** (helper / revoked bytes)  
3. Wipe published / wipe object (actual delete)

Matches separate realm trees (C004).

### U8 — Import from OS (optional later)

Pick a filesystem file → ingest as **private** by default. Becoming shareable still requires explicit Share publicly…. Library listing remains CAS-only, not a live OS browser.

### U9 — Link / tip share (no open shelf)

User shares one public `content_id` (or short tip) via chat, QR, or clipboard. Recipients fetch when a provider (peer, Node, or CDN assist) is available. Does **not** require directory `content_library` or a browsable catalog.

### U10 — Open library (Node-gated)

User enables “Publish kept-public library…” only when a **stable serve** path exists: reachable desktop Node, Home Node, or `pp-node` with `blob_provide` / `content_library`.

- Directory: advertise capability (+ optional manifest tip) — **not** the full file list  
- Serving peer: answers catalog list for **pinned public** only (never private, never unpinned cache)  
- ACL v1 preference: Contacts or Link-manifest; fully open internet browse is opt-in later  
- Without stable serve: control disabled / explained (“Needs a reachable Node or Home Node”)

### U11 — CDN / storage assist

Optional HTTP/CDN path for public objects while the owner is offline (**C009**). Under current relay doctrine (**R002**), this is **delivery assist** with GC — local CAS or Home Node remains source of truth. A durable **hosted shelf** (mode D) needs its own retention/quota decision; do not upgrade relay blob to archive by accident.

---

## Default pin policy (C013)

| Event | Default |
|-------|---------|
| New private attachment / import | **Pinned** (Kept) |
| Explicit Share publicly… | Public row **Pinned** |
| Peer-fetched public object | **Unpinned** (Cache) |
| Unpublish | Unpin + stop provide |
| Re-keep from Cache | User Pin → Kept (advanced) |

---

## Non-goals (use-case layer)

- Stuffing per-file catalogs into person / mesh directory records  
- Requiring a stable Node for one-shot link or contact share (U9 / U8+U4)  
- Treating “pin” as the privacy boundary (realm is)  
- OS-wide file manager UX as the primary library  
- Bitswap / Kubo wire compatibility (see DESIGN non-goals)
