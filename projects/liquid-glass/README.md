# Floating Chrome — compact shell chrome

**Status:** **Done** (2026-07-22)  
**Stable refs:** [docs/ui/UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials), [docs/ui/WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md#compact-floating-chrome), [docs/ui/RCSS_PROFILE.md](../../docs/ui/RCSS_PROFILE.md)

Former name: **Liquid Glass**. Pivoted per [LG008](DECISIONS.md#lg008--floating-chrome-top-layer-frost-only).

## Outcome

Compact mobile shell uses **Floating Chrome**: floating inset geometry, opaque elevated surfaces, and **at most one** `backdrop-filter` per frame on the topmost chrome bar. Expanded desktop chrome unchanged.

## Project archive

| Doc | Role |
|-----|------|
| [DESIGN.md](DESIGN.md) | Exploration spec (superseded by stable docs for normative rules) |
| [DECISIONS.md](DECISIONS.md) | ADRs LG001–LG008 |
| [PHASES.md](PHASES.md) | Delivery checklist (complete) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Shipped summary + perf gate devices |

Do not edit normative tables in both this folder and `docs/ui/` — stable docs win.
