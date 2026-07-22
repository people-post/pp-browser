# Liquid Glass — compact shell chrome

**Status:** **In progress** — lg2–lg3 materials + shape (LG003=C); visual QA deferred  
**Owner:** TBD  
**Stable refs:** [docs/ui/UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md), [docs/ui/WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md), [docs/ui/RCSS_PROFILE.md](../../docs/ui/RCSS_PROFILE.md), [docs/architecture/RMLUI_UPSTREAM.md](../../docs/architecture/RMLUI_UPSTREAM.md)

## One-line goal

Bring **Apple-faithful Liquid Glass** to pp-browser **compact layout** shell controls (bottom nav, overlay headers, sheets, composer chrome) while keeping expanded desktop productivity chrome unchanged and providing opaque fallbacks where GPU or accessibility requires it.

## Why deferred

This is a **renderer + layout + design-system** project, not a theme tweak. Ship when:

- Compact mobile shell is stable on iOS/Android (see [docs/ops/IOS_BUILD.md](../../docs/ops/IOS_BUILD.md) when iOS lands).
- There is budget for cross-platform GPU profiling and fork maintenance.
- Product accepts a visual shift away from pure Notion/Slack flat surfaces on mobile (see LG001 in [DECISIONS.md](DECISIONS.md)).

## Release scope (target)

| In | Out |
|----|-----|
| Compact-only glass chrome (`layout_mode == compact`) | Expanded desktop nav / panes restyle |
| Bottom nav pill, chat overlay header, transient header, auxiliary sheet header | Every bubble, list row, or settings field |
| Real `backdrop-filter` blur + translucency | Native `UIVisualEffectView` on iOS (v1) |
| Scroll-through sampling (floating overlay layout) | Full system-wide material registry |
| Reduce-transparency / low-GPU fallback | Pixel-perfect iOS 26 parity on day one |
| Design tokens + RCSS profile notes for agents | AI-generated glass on arbitrary widgets |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Visual spec, surfaces, layout rules, perf/accessibility |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Baseline today (pre-glass) |
| [PHASES.md](PHASES.md) | Phased delivery checklist — start here when resuming |
| [DECISIONS.md](DECISIONS.md) | ADRs (LG001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| lg0 | Project docs + ADRs | Done |
| lg1 | Layout foundation (floating compact chrome) | Done |
| lg2 | Frosted glass materials (RCSS tokens + backdrop-filter) | Done (visual QA deferred) |
| lg3 | Liquid shape + edge treatment (LG003=C) | Done (visual QA deferred) |
| lg4 | Motion + scroll coupling | Not started |
| lg5 | Renderer extensions (specular / advanced material) | Not started |
| lg6 | Fallbacks, perf gates, agent docs | Not started |
| lg7 | Promote to `docs/ui/` | Not started |

## When you are ready

1. Read [CURRENT_STATE.md](CURRENT_STATE.md) and [DESIGN.md](DESIGN.md).
2. Resolve open items in [DECISIONS.md](DECISIONS.md) (LG004 after visual review; LG005–LG007 before lg6).
3. Execute [PHASES.md](PHASES.md) from lg4/lg6; do not start lg5 until lg2–lg3 screenshots are reviewed.
