# Content CAS — current state

**As of:** 2026-09-05

| Phase | Status |
|-------|--------|
| **P0** — contracts | **Done** |
| **P1** — private CasStore + ObjectIndex | **Done** (public clear put/get included for realm isolation; no publish UX) |
| **P2** — big-bang attachment cutover | **Done** (AttachmentCache → private CAS only; legacy removed) |
| **P3** — public publish UX + library | Not started |
| **P4** — provide/fetch | Not started |
| **P5** — pieces | Not started |

## P1 landed

| Piece | Path |
|-------|------|
| Types / realms | `src/domain/messaging/CasTypes.h` |
| Index | `ObjectIndex.*` → `{profile}/object_index.db` |
| Store | `CasStore.*` → `cas/{private\|public}/blocks/{aa}/{bb}/{content_id_hex}` (C010) |
| Tests | `tests/cas_store_test.cpp` (5 cases) |

Private blocks: PPBA + `FileCipher` under profile DEK (AAD `cas-private\|{profile_id}\|{content_id_hex}\|1`). Public blocks: clear bytes. Content id = BLAKE2b-256(plaintext) (R016). Attachment durable path is private CAS only (C007); legacy `blobs/` support removed.


## P2 landed

| Piece | Change |
|-------|--------|
| Durable save/load | `SaveAttachmentPlaintext` / `LoadAttachmentPlaintext` require DEK + `profile_id`; private CAS only |
| Legacy | **Removed** — no thread `blobs/` read/write path, no migrate helper |
| Views | `blobs_view/` session plaintext only (materialized from CAS) |
| Wipes | Per-thread wipe clears views/pending cipher (+ orphan `blobs/` dir if present); clear-all wipes `cas/private` |
| Layout | Blocks `cas/private/blocks/{aa}/{bb}/{id}` (C010) |

## Private presentation (post-P2, C011)

| Piece | Change |
|-------|--------|
| RAM LRU | `AttachmentPlaintextMemoryCache` — private plaintext (≤8 MiB/entry, 64 entries / 64 MiB); filled on save/load; **cleared on ClearDek** with `WipeAllAttachmentViewCaches` |
| Inline video gate | `kMaxInlinePrivateVideoBytes` (= Soft 4 MiB): large private videos stay in CAS; soft poster only; no `blobs_view` until explicit open |
| Images / posters | Prefer RAM then CAS; `blobs_view` still used for RmlUi file `src` (future: in-memory texture / stream decrypt) |

## Module home (C012)

| Now | Later |
|-----|-------|
| Implementation under `src/domain/messaging/` (`CasStore`, `ObjectIndex`, `AttachmentCache`, `AttachmentPlaintextMemoryCache`, …) | Peel to `src/domain/content/` at P3/P4 when public share/library/provide-fetch needs CAS without linking messaging |
| Chat-specific download/suppression/responders stay messaging/feature | Feature wires `common` contracts across peers |

