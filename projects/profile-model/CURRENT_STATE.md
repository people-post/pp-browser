# Profile model — current state

**As of:** 2026-11-23

P0: inventory written ([INVENTORY.md](INVENTORY.md)); PM001–PM010 accepted by Hongwei (2026-11-23). Release 1 builds the
whole layout with one backend per profile (PM009). In [DESIGN § Open questions](DESIGN.md#open-questions), 2–3 are
defaults and 4–6 wait for several backends per profile; P0 is done.
Nothing is built: the app still keeps one profile per backend
([DATA_LAYOUT § Per-backend profiles](../../docs/contracts/DATA_LAYOUT.md#per-backend-profiles)), and #733 stays open
until P1. The issues found in the inventory are #738–#751.
