# Floating Chrome — current state

**Last updated:** 2026-07-22  
**Implementation:** lg1–lg3 + lg8 (Floating Chrome pivot)

## Shipped

| Surface | Treatment |
|---------|-----------|
| Bottom nav | Twin floating pills; **opaque** `.surface-chrome` fill + elevation shadow; **frost** (`blur(12px)`) only when bottom nav is top chrome |
| Chat overlay header | Opaque chrome; frost when chat overlay is top interruption |
| Transient header | Opaque chrome; frost when transient layer is top |
| Auxiliary sheet | **Opaque** body (`surface-elevated`); 8dp top strip; frost on strip when sheet is top |
| Account sheet | Opaque body; header bar frost when account sheet is top |
| Home / overlay composer | Opaque `.surface-chrome` strip (never frost) |

### Frost selection (C++)

`ShellInterruption::CompactChromeFrostSurface(state)` picks **at most one** bar per frame from the interruption stack. Modals (dialog, overlay, pin gate) → no frost.

### Tokens / utilities

| Item | Location |
|------|----------|
| Chrome fills / borders / shadows | `assets/themes/colors-light.rcss`, `colors-dark.rcss` |
| `.surface-chrome` / `.surface-chrome--frost` / `.surface-chrome--solid` | `components.rcss` + theme colors |
| Frost class wiring | `ShellHost::SerializeCompactBase`, `SerializeTransientLayer`, `SerializeAccountSheet` |

**Not yet:** reduce-transparency pref wiring (lg6), perf gates (lg6), stable `docs/ui/` promotion (lg7).

## Rendering

| Capability | Status |
|------------|--------|
| `backdrop-filter: blur()` | **One surface max** via `--frost` or `shell-bottom-chrome--frost` |
| Custom glass shaders (lg5) | Cancelled |

## Next agent — start here

Continue with **lg6** (fallbacks + perf) in [PHASES.md](PHASES.md). Resolve LG005 / LG006 / LG007 before lg6 merge gates.
