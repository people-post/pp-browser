# Content CAS — phases

Ordering only. Status: [CURRENT_STATE.md](CURRENT_STATE.md).

| Phase | Intent | Exit |
|-------|--------|------|
| **P0** | Contracts: DESIGN/DECISIONS + DATA_LAYOUT sketch + L4 share/broadcast rows | Docs merged |
| **P1** | `CasStore` + `ObjectIndex` for **private** realm (DEK wrap) | Unit tests; no attachment cutover yet |
| **P2** | **Big-bang** attachment cutover (C007): write/read private CAS only; drop thread `blobs/` + migrate helper | Attachment fetch/send green; no legacy `blobs/` I/O |
| **P3** | **Public** realm + Share publicly… + library UI | Publish/unpublish; no silent promote; local Kept/Cache indicators (C013) |
| **P4** | Provide/fetch public ids (peer blob ± optional CDN path); link/tip share | End-to-end public share without requiring open catalog |
| **P4b** | Node-gated open library: capability on directory + catalog rpc on serving PeerId/Home Node | U10; directory still holds no per-file lists |
| **P5** | Optional piece/manifest mode inside realms | Multi-source assemble |

P0 is this change set. P1+ are implementation.

Post-P2 (same branch as cutover): private presentation policy **C011** (RAM LRU + inline video size gate) — see [DECISIONS](DECISIONS.md#c011--private-presentation-ram-lru--video-size-gate).

**C012:** keep CAS under `domain/messaging` through P2/C011; peel to `domain/content` when P3/P4 introduces a second non-messaging owner — see [DECISIONS](DECISIONS.md#c012--module-home-stay-in-messaging-until-public-cas-has-a-second-owner).

**C013:** pin defaults + share modes (link/contact before open library); use cases in [USE_CASES.md](USE_CASES.md). Durable hosted shelf remains an explicit later product, not a silent CDN archive.

**P3 thin slice (landed):** `ObjectIndex::List`, `PublishFromPrivate` / `Unpublish`, Me → Storage Files library (filters + Share publicly… / Unpublish…). Provide/fetch and Node-gated open catalog remain P4/P4b.

**P4 thin slice (landed):** `pp-cas:v1:` tip Format/Parse; Me → Storage Copy tip… / Fetch tip…; `/pp-browser/blob/1.0.0` op `fetch_public` serves **Kept public** clear bytes only (Unpublish stops provide); peer fetch stores **public Cache**. CDN assist and Node-gated open catalog remain later/P4b.
