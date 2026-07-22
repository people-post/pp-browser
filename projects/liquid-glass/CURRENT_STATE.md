# Liquid Glass — current state

**Last updated:** 2026-07-21  
**Implementation:** lg2 + lg3 landed (LG003 = C); visual QA deferred

## Shipped (lg1–lg3)

| Surface | Treatment |
|---------|-----------|
| Bottom nav | Floating pill (`border-radius` 24dp, inset 12dp); `backdrop-filter: blur(20px)` + glass fill; active tab capsule 12dp |
| Chat overlay header | Full-bleed frosted bar; absolute over messages (`padding-top` 48dp on body) |
| Transient header | Same glass material; absolute over transient pane |
| Auxiliary sheet | Full-body glass (LG003=C); lighter scrim tint |
| Home / overlay composer | Glass strip on `#shell-composer-mount` (`surface-glass`); softer composer card |

### Tokens / utilities

| Item | Location |
|------|----------|
| Glass fills / borders / shadows | `assets/themes/colors-light.rcss`, `colors-dark.rcss` |
| `.surface-glass` / `.surface-glass--opaque` | `components.rcss` + theme colors |
| Markup classes | `ShellHost::SerializeCompactBase`, `SerializeTransientLayer` |

**Not yet:** scroll-coupled motion (lg4), custom specular shader (lg5), reduce-transparency wiring / perf gates (lg6), stable `docs/ui/` promotion (lg7).

## Rendering capabilities (relevant)

| Capability | Status |
|------------|--------|
| `backdrop-filter: blur()` | Used on compact chrome |
| `filter: shader(...)` | Experimental — deferred (lg5; only if visual review fails) |

## Next agent — start here

Continue with **lg4** (optional polish) or **lg6** (fallbacks + perf) in [PHASES.md](PHASES.md). Resolve LG005 / LG006 / LG007 before lg6 merge gates. Do not start lg5 until side-by-side iOS review of lg2–lg3.
