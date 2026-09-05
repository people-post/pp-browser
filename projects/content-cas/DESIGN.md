# Content CAS — design

**Status:** Accepted design (2026-09-05)  
**Decisions:** [DECISIONS.md](DECISIONS.md)  
**Phases:** [PHASES.md](PHASES.md)

## Problem

Chat attachments are **E2E ciphertext on the wire**, decrypted in memory, then **re-encrypted at rest** under the profile DEK (PPBA / `blobs_view/`). Public file sharing must not blur into that path: “I can decrypt this locally” must never mean “this is public.”

We also want one on-disk engine that can later support piece/multi-source fetch and optional chain/DVR bodies without a third store type.

## Solution

**One CAS implementation, two realms:**

| Realm | Wire path | At rest | Object id |
|-------|-----------|---------|------------|
| **private** | E2E / chat-blob relationship | DEK-wrapped blocks | BLAKE2b-256(**plaintext**) — R016/D075 |
| **public** | Explicit publish; blob fetch by public id | Clear published bytes (v1) | Hash of **published** bytes; new object even if plaintext matches private |

```text
{profile}/
  cas/
    private/blocks/{aa}/{bb}/{content_id_hex}  # DEK-wrapped; two-level hex shard (C010)
    public/blocks/{aa}/{bb}/{content_id_hex}   # clear published (v1); same layout
  object_index.db       # realm, content_id, mime, pins, published_from?, refs
  threads/{id}/
    …                   # after cutover: refs / session views only — not a second byte store
```

Durable bytes live in `cas/private` only. Thread `blobs_view/` is a session plaintext cache; per-thread `blobs/` is not used.

## Invariants

1. Chat decrypt / attachment save **never** writes `cas/public/`.
2. Public share requires an explicit **Share publicly…** publish op (confirm-leading UX).
3. APIs take **`realm` always** — no ambient default CAS root.
4. No cross-realm block-file sharing in v1 (C005).
5. L4 kinds unchanged: share = discover + blob + rpc; live broadcast stays realtime ([L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md)).

## Publish flow

```text
private object (plaintext id P)
  → user confirms Share publicly…
  → load private plaintext in memory (DEK)
  → encode published payload (v1 = plaintext bytes)
  → content_id_public = hash(published_bytes)
  → write cas/public/blocks/{aa}/{bb}/{id}
  → index row realm=public, published_from=P
  → optional announce (P4)
```

Revoke / unpublish: stop provide + unpin; GC may delete later. Revoke ≠ secure delete unless user chooses wipe (C006).

## API sketch (P1+)

```text
CasStore::Put(realm, content_id, bytes, meta)   # private: wrap with DEK
CasStore::Get(realm, content_id) -> bytes       # private: unwrap
CasStore::Exists / Delete
ObjectIndex::Upsert / Lookup / ListPins / SetPublishedFrom
Attachment path: Put private → index + thread ref (no dual-write — C007)
Publish: Get private → Put public → index
```

## Delivery paths (orthogonal)

Private and public blob **conversations** may use peer OPEN, circuit, or HTTP/CDN. CDN is an optional path for public objects (C009), not a second store.

## Non-goals (this project)

- Bitswap / Kubo wire compatibility  
- Link-scoped encryption for public objects (deferred; C001)  
- Rarest-first piece swarm (P5 later)  
- Live broadcast media filesystem (realtime; DVR may land as public/private objects later)
