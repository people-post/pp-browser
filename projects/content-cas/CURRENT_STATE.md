# Content CAS — current state

**As of:** 2026-09-05

| Phase | Status |
|-------|--------|
| **P0** — contracts | **Done** |
| **P1** — private CasStore + ObjectIndex | **Done** (public clear put/get included for realm isolation; no publish UX) |
| **P2** — big-bang attachment cutover | **Done** (AttachmentCache → private CAS; unlock migrate) |
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

Private blocks: PPBA + `FileCipher` under profile DEK (AAD `cas-private\|{profile_id}\|{content_id_hex}\|1`). Public blocks: clear bytes. Content id = BLAKE2b-256(plaintext) (R016). Attachment durable path cut over in P2 (C007).


## P2 landed

| Piece | Change |
|-------|--------|
| Durable save/load | `SaveAttachmentPlaintext` / `LoadAttachmentPlaintext` use `CasStore` private when DEK set |
| Legacy | No-dek saves still write thread `blobs/` (fixtures); reads fall back to legacy |
| Migrate | `MigrateLegacyAttachmentBlobsToCas` on unlock (`AttachmentDownloadService::SetDek`) |
| Views | `blobs_view/` unchanged (session plaintext) |
| Wipes | Per-thread wipe clears views/legacy/cipher; clear-all also wipes `cas/private` |
| Layout | Blocks remain `cas/private/blocks/{aa}/{bb}/{id}` (C010) |
