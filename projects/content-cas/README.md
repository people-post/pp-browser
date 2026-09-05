# Content CAS (private / public realms)

**Status:** Design accepted (2026-09-05); P1–P2 landed; private presentation (C011) landed; module home locked (C012) — see [CURRENT_STATE](CURRENT_STATE.md)  
**Normative disk sketch:** [DATA_LAYOUT § Content CAS](../../docs/contracts/DATA_LAYOUT.md#content-cas-planned)  
**L4 composition:** [L4_PROTOCOL_KINDS § Prepared compositions](../../docs/contracts/L4_PROTOCOL_KINDS.md#prepared-compositions-no-new-kinds)  
**Related:** [relay-blob-upload](../relay-blob-upload/) (attachments MVP shipped), [at-rest-crypto](../at-rest-crypto/), [peer-scoped-broadcast](../peer-scoped-broadcast/) (announce tips → optional DVR in CAS)

One content-addressed store engine with **two confidentiality realms**: chat attachments (E2E → memory → DEK wrap) vs explicit public publish. No new L4 kinds.

| Doc | Role |
|-----|------|
| [DESIGN.md](DESIGN.md) | Architecture, invariants, APIs |
| [DECISIONS.md](DECISIONS.md) | C001–C012 locked choices |
| [PHASES.md](PHASES.md) | P0–P5 delivery order |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Progress |
