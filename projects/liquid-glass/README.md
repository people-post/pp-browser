# Floating Chrome — compact shell chrome

**Status:** **In progress** — lg6 fallbacks next  
**Owner:** TBD  
**Stable refs:** [docs/ui/UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md), [docs/ui/WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md), [docs/ui/RCSS_PROFILE.md](../../docs/ui/RCSS_PROFILE.md)

Former project name: **Liquid Glass** (Apple-faithful materials). Pivoted 2026-07-22 — see [LG008](DECISIONS.md#lg008--floating-chrome-top-layer-frost-only).

## One-line goal

Bring **Floating Chrome** to pp-browser **compact layout** shell controls: floating inset pills, elevated opaque surfaces, and **at most one** backdrop-frost bar per frame on the topmost visible chrome — while expanded desktop chrome stays flat.

## Why this direction

Apple-faithful multi-layer `backdrop-filter` is GPU-heavy in RmlUi (each glass surface re-samples and blurs content every frame). Floating layout (lg1) already delivers most UX value; **top-layer frost only** keeps a hint of depth without stacking blur passes.

## Release scope (target)

| In | Out |
|----|-----|
| Compact-only chrome (`layout_mode == compact`) | Expanded desktop nav / panes restyle |
| Floating bottom nav pills + overlay/transient headers | Every bubble, list row, or settings field |
| Opaque `.surface-chrome` on all compact chrome bars | Multi-surface simultaneous blur |
| Single `surface-chrome--frost` on top interruption chrome | Apple iOS 26 parity |
| lg1 overlay layout + safe area | lg4 scroll-coupled motion |
| Reduce-transparency → `.surface-chrome--solid` | lg5 custom shaders |
| Design tokens + RCSS profile notes for agents | AI-generated chrome on arbitrary widgets |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Visual spec, surfaces, frost rules, perf/accessibility |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Baseline today |
| [PHASES.md](PHASES.md) | Phased delivery checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (LG001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| lg0 | Project docs + ADRs | Done |
| lg1 | Layout foundation (floating compact chrome) | Done |
| lg2 | Chrome materials (RCSS tokens) | Done — pivoted to opaque default |
| lg3 | Floating shape + edge treatment | Done |
| lg8 | Floating Chrome pivot (top-layer frost only) | Done |
| lg4 | Motion + scroll coupling | **Cancelled** (LG008) |
| lg5 | Renderer extensions (specular shader) | **Cancelled** (LG008) |
| lg6 | Fallbacks, perf gates, agent docs | Not started |
| lg7 | Promote to `docs/ui/` | Not started |

## When you are ready

1. Read [CURRENT_STATE.md](CURRENT_STATE.md) and [DESIGN.md](DESIGN.md).
2. Execute [PHASES.md](PHASES.md) lg6 → lg7.
3. Resolve LG005–LG007 before lg6 merge gates.
