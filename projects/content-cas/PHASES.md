# Content CAS — phases

Ordering only. Status: [CURRENT_STATE.md](CURRENT_STATE.md).

| Phase | Intent | Exit |
|-------|--------|------|
| **P0** | Contracts: DESIGN/DECISIONS + DATA_LAYOUT sketch + L4 share/broadcast rows | Docs merged |
| **P1** | `CasStore` + `ObjectIndex` for **private** realm (DEK wrap) | Unit tests; no attachment cutover yet |
| **P2** | **Big-bang** attachment cutover (C007): migrate + write only CAS private; thread refs/views | Attachment fetch/send green; old durable `blobs/` gone |
| **P3** | **Public** realm + Share publicly… + library UI | Publish/unpublish; no silent promote |
| **P4** | Provide/fetch public ids (peer blob ± optional CDN path) | End-to-end public share |
| **P5** | Optional piece/manifest mode inside realms | Multi-source assemble |

P0 is this change set. P1+ are implementation.
