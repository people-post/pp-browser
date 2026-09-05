# Content CAS — current state

**As of:** 2026-09-05

| Phase | Status |
|-------|--------|
| **P0** — contracts | **Done** |
| **P1** — private CasStore + ObjectIndex | **Done** (public clear put/get included for realm isolation; no publish UX) |
| **P2** — big-bang attachment cutover | Not started |
| **P3** — public publish UX + library | Not started |
| **P4** — provide/fetch | Not started |
| **P5** — pieces | Not started |

## P1 landed

| Piece | Path |
|-------|------|
| Types / realms | `src/domain/messaging/CasTypes.h` |
| Index | `ObjectIndex.*` → `{profile}/object_index.db` |
| Store | `CasStore.*` → `cas/{private\|public}/blocks/{content_id_hex}` |
| Tests | `tests/cas_store_test.cpp` (5 cases) |

Private blocks: PPBA + `FileCipher` under profile DEK (AAD `cas-private\|{profile_id}\|{content_id_hex}\|1`). Public blocks: clear bytes. Content id = BLAKE2b-256(plaintext) (R016). Attachment cutover still pending (C007).
