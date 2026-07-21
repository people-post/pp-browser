# Liquid Glass — current state

**Last updated:** 2026-07-21  
**Implementation:** lg1 in progress (layout foundation)

## Baseline (pre-lg1)

Compact shell chrome was **opaque and stacked**, not glass.

| Surface | Location | Previous behavior |
|---------|----------|-------------------|
| Bottom nav | `ShellHost::SerializeCompactBase()`, `.shell-bottom-chrome`, `.shell-nav-rail--compact` | Flex sibling **below** content; solid bg + top border |
| Nav tabs | `assets/views/nav_rail.rml`, `.shell-nav-tab` | 52×56dp targets; filled active state |
| Chat overlay header | `.shell-chat-overlay-chrome` | Solid bar + back button |
| Transient header | `.shell-transient-chrome` | Solid back row |
| Auxiliary sheet | `.shell-sheet-compact` | Opaque panel above nav; scrim `#00000099` |
| Composer (Home compact) | `#shell-composer-mount` | Standard elevated surface |

## lg1 — layout foundation (this PR)

| Change | Location | Behavior |
|--------|----------|----------|
| Floating bottom chrome | `.shell-bottom-chrome` | `position: absolute`; horizontal inset 12dp; overlays content |
| Content scroll padding | `.shell-nav-page`, `ShellHost::ApplyCompactChromeLayout()` | `padding-bottom`: 56dp nav + safe-area bottom |
| Safe area | `ShellHost::RefreshSafeAreaInsets()`, `machine.json` `safe_area.bottom` | SDL `SDL_GetWindowSafeArea` with prefs fallback |
| Auxiliary sheet offset | `#shell-auxiliary-sheet` | Bottom offset matches nav + safe area |

**Not yet:** `backdrop-filter`, glass tokens, pill radius, motion, shaders (lg2–lg5 deferred).

## Rendering capabilities (relevant)

| Capability | Status |
|------------|--------|
| `backdrop-filter: blur()` | Supported in RmlUi fork + `RmlUi_Renderer_GL3` — **not used in app yet** |
| `filter: shader(...)` | Experimental — deferred (lg5) |

## Next agent — start here

After lg1 merges, continue with **lg2** in [PHASES.md](PHASES.md): frosted glass RCSS tokens + `backdrop-filter` on compact chrome surfaces. Do not start lg5 until lg2 screenshots are reviewed.
