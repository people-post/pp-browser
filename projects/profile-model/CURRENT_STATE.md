# Profile model — current state

**As of:** 2026-11-24

P0: inventory written ([INVENTORY.md](INVENTORY.md)); PM001–PM012 accepted by Hongwei (2026-11-23). Release 1 builds the
whole layout; a profile has exactly one home backend (PM009, PM012). In [DESIGN § Open questions](DESIGN.md#open-questions), 2–3 are
defaults and 4–6 are closed by PM012 (one home backend per profile, permanently); P0 is done.
Nothing is built: the app still keeps one profile per backend
([DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles)), and #733 stays open
until P1. The issues found in the inventory are #738–#751; the phases are tracked in #752–#757.

**P2a (backend tables) is built** (PR D, `kenneth/pm-backend-tables`; nothing else of P2): the backend type
(`BackendTable`), the built-in tables compiled from `CompiledOrigins`, a custom table read from `config.json` with optional
keys only (`backend.services`; `config_version` stays 1), every caller taking its address from the active table, a service
the table lacks absent with no fallback (#741, #742, #743, #744 fixed), and PM010's custom id as a function that profile
selection does not use yet. Still open in P2: `services.json` and the Network page's table editor (P2b).
