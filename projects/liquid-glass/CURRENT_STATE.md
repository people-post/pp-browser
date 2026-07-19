# Liquid Glass — current state

**Last updated:** 2026-07-19  
**Implementation:** none (planning only)

## Baseline (today)

Compact shell chrome is **opaque and stacked**, not glass.

| Surface | Location | Current behavior |
|---------|----------|------------------|
| Bottom nav | `ShellHost::SerializeCompactBase()`, `.shell-bottom-chrome`, `.shell-nav-rail--compact` | Flex sibling **below** content; solid bg + top border |
| Nav tabs | `assets/views/nav_rail.rml`, `.shell-nav-tab` | 52×56dp targets; filled active state |
| Chat overlay header | `.shell-chat-overlay-chrome` | Solid bar + back button |
| Transient header | `.shell-transient-chrome` | Solid back row |
| Auxiliary sheet | `.shell-sheet-compact` | Opaque panel above nav; scrim `#00000099` |
| Composer (Home compact) | `#shell-composer-mount` | Standard elevated surface |

Theme: productivity flat palette in `assets/themes/colors-light.rcss` / `colors-dark.rcss` ([UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md) — Notion/Slack aesthetic).

## Rendering capabilities (relevant)

| Capability | Status |
|------------|--------|
| `backdrop-filter: blur()` | Supported in RmlUi fork + `RmlUi_Renderer_GL3` (desktop + GLES) |
| `filter: blur()` etc. | Supported |
| `filter: shader(...)` | Experimental in renderer — not used in app |
| `background-image` / gradients in RCSS | **Not supported** for AI/app chrome (RCSS profile) |
| Pseudo-elements (`::before`) | **Not supported** |

No `backdrop-filter` usage in `assets/` yet.

## Layout constraint (blocks “real” glass today)

In compact mode the bottom nav is the **last flex child** in `.shell-layer-compact`. Content scrolls **above** the bar in layout space, not **under** it. `backdrop-filter` on the nav would sample mostly app background, not scrolling chat/list pixels.

Fix required before glass reads correctly: **floating overlay chrome** + content `padding-bottom` / safe-area inset (see lg1 in [PHASES.md](PHASES.md)).

## Platform notes

| Platform | Notes |
|----------|-------|
| Desktop | Compact mode at width &lt;768dp; good dev testbed via resize |
| Android | OpenGL ES 3; blur cost unknown — must profile on mid-tier device |
| iOS | Same GLES path when shipped; safe-area bottom inset needed for home indicator |
| Simulated touch | `-DRMLUI_BACKEND_SIMULATE_TOUCH=ON` — use for compact QA on Linux |

## Related work (not this project)

- iOS scaffold: `docs/ops/IOS_BUILD.md` (separate track)
- Expanded shell / working set: [WORKING_SET_PANEL.md](../../docs/ui/WORKING_SET_PANEL.md)
- Native iOS `UIVisualEffectView`: out of scope for v1 (LG002)

## Next agent — start here

When implementation begins, start with **lg1** in [PHASES.md](PHASES.md): restructure compact bottom chrome to absolute/floating overlay without changing nav behavior or interruption stack order ([WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md)).
