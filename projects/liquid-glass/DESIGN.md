# Liquid Glass — design

Apple’s **Liquid Glass** (iOS 26 / system materials) combines **translucency**, **backdrop blur**, **specular edge lighting**, **floating pill geometry**, and **motion** that responds to scroll and context. pp-browser targets **faithful behavior within RmlUi constraints**, not a pixel-perfect shader clone on every GPU.

## Scope boundary

### In scope (compact shell chrome)

| Surface | RML / C++ | Glass treatment |
|---------|-----------|-----------------|
| Bottom nav rail | `nav_rail.rml`, `.shell-nav-rail--compact` | Twin floating pills (Home | rest); blur + tint; active tab luminance |
| Bottom chrome wrapper | `.shell-bottom-chrome` | Overlay positioning; safe-area padding |
| Chat overlay header | `.shell-chat-overlay-chrome` | Frosted bar over scrolling messages |
| Transient back header | `.shell-transient-chrome` | Same material as overlay header |
| Auxiliary sheet header area | `.shell-sheet-compact` top region | Frosted header strip; body may stay opaque v1 |
| Home composer chrome (optional lg3) | `#shell-composer-mount` wrapper | Light glass strip separating composer from chat |

### Out of scope (v1)

- Expanded layout (≥768dp) nav column and pane dividers
- Chat bubbles, sidebar rows, settings lists, dialogs (keep existing surfaces)
- Native platform materials (`UIVisualEffectView`, Android `RenderEffect` blur outside RmlUi)
- AI-generated arbitrary glass widgets
- `background-image` gradients (unsupported in RCSS profile)

## Visual principles (Apple-faithful targets)

1. **Material reads content behind it** — scrolling text/icons blur through the control, not a flat tint on empty background.
2. **Floating hierarchy** — primary navigation sits inset from screen edges (horizontal margin + bottom safe area), not edge-to-edge slab.
3. **Luminance-adaptive tint** — light mode: frosted white ~60–75% opacity; dark mode: frosted charcoal ~50–65%; avoid saturated fills.
4. **Specular edge** — 1dp highlight on top/leading edge (simulated via border-color or future shader); soft outer shadow optional.
5. **Legibility** — icons and labels maintain WCAG-ish contrast on glass; active tab uses filled capsule, not only color shift.
6. **Motion** — subtle opacity/blur strength change when content scrolls under chrome (lg4); respect reduced motion.

Reference: Apple Human Interface Guidelines — Materials; compare against iOS 26 tab bar and navigation bars in Simulator when tuning.

## Layout architecture change (required)

Current compact tree (simplified):

```
.shell-layer-compact (flex column)
  .shell-nav-page          ← flex:1 content
  .shell-chat-overlay?     ← absolute overlay when open
  .shell-sheet-compact?    ← sheet + scrim
  .shell-bottom-chrome     ← flex-shrink:0 (NOT overlapping)
```

Target tree:

```
.shell-layer-compact (position relative, flex column)
  .shell-nav-page          ← flex:1; padding-bottom: nav_height + safe_area
  .shell-chat-overlay?     
  .shell-sheet-compact?    
  .shell-bottom-chrome     ← position:absolute; bottom:0; left/right: inset; z-index:15
```

Content panes must extend **under** the glass nav (visually) while keeping **scroll padding** so the last message / list row is not obscured.

### Safe area

- Read bottom inset from existing platform/shell path (SDL safe area / config `safe_area` prefs).
- Apply to `.shell-bottom-chrome` `bottom` offset and `.shell-nav-page` `padding-bottom`.
- Document in [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md) when shipped.

### Interruption stack

Glass must not change dismiss order ([WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md)): dialog → overlay → transient → auxiliary sheet → compact chat overlay → base panes. Only **presentation** changes.

## Material specification (RCSS tokens)

Add to `colors-light.rcss` / `colors-dark.rcss` inside theme blocks (no CSS variables — literal hex/rgba per existing pattern).

| Token | Light (initial) | Dark (initial) | Use |
|-------|-----------------|----------------|-----|
| `glass-fill` | `#ffffffb3` | `#1f2937b3` | Background on glass elements |
| `glass-fill-strong` | `#ffffffd9` | `#1f2937d9` | Reduce-transparency fallback |
| `glass-border-highlight` | `#ffffff66` | `#ffffff1a` | Top edge specular |
| `glass-border-subtle` | `#00000014` | `#ffffff0f` | Outer separation |
| `glass-blur-radius` | `20px` | `20px` | `backdrop-filter: blur(20px)` |
| `glass-shadow` | box-shadow | box-shadow | Optional elevation |

Utility class (author theme, not AI-generated):

```css
.surface-glass {
  background-color: /* glass-fill */;
  backdrop-filter: blur(20px);
  border-top: 1dp /* glass-border-highlight */;
  border-radius: 22dp;
}
.surface-glass--opaque {
  backdrop-filter: none;
  background-color: /* glass-fill-strong or surface-elevated */;
}
```

Tune values against real chat scroll capture on device.

## Component-level spec

### Bottom nav pills

| Property | Target |
|----------|--------|
| Height | 56dp + safe-area bottom |
| Horizontal inset | 12–16dp from screen edges |
| Layout | Two pills: primary (Home) left, secondary (Sessions / Contacts / Me) right |
| Corner radius | 22–28dp (full pill) per glass cluster |
| Tab active state | Capsule fill inside pill (`border-radius: 12dp`) |
| Badges | Unchanged logic; ensure contrast on glass |

DOM: `.shell-nav-pill--primary` / `.shell-nav-pill--secondary` inside `.shell-nav-rail-inner`; glass on pills in compact only.

### Chat / transient headers

| Property | Target |
|----------|--------|
| Height | 48–52dp |
| Width | Full bleed top of overlay (or inset pill v2) |
| Blur | Same token as nav |
| Back button | Existing `.shell-back-btn`; 44dp target |

### Auxiliary sheet (compact)

| Property | Target |
|----------|--------|
| v1 (LG003=C) | Full-height glass body + lighter scrim tint |
| Earlier option B | Frosted **header strip** only; body `surface-elevated` |

## Renderer path (lg5 — Apple-faithful extras)

Stock RmlUi `backdrop-filter: blur()` covers **frosted glass**. Apple **Liquid** emphasis adds:

- Directional specular highlight (light source from top)
- Slight chromatic edge / refraction feel on curved regions
- Optional dynamic blur radius from scroll velocity

Implementation options (pick in LG004):

| Option | Fidelity | Cost |
|--------|----------|------|
| A. RCSS only (blur + borders + shadow) | Medium | Low |
| B. Custom `filter: shader(glass, ...)` in GL3 | High | Medium — fork + maintain |
| C. Platform native overlay views | Highest on iOS | High — breaks single RmlUi tree |

**Recommendation:** A for lg2–lg3, B for lg5 if A fails visual review on iOS Simulator side-by-side.

### Fork touchpoints (option B)

- `src/render/integration/renderer/RmlUi_Renderer_GL3.cpp` — `CompileFilter` / shader path
- `src/render/fork/Source/Core/ElementEffects.cpp` — backdrop sampling
- Document in [RMLUI_UPSTREAM.md](../../docs/architecture/RMLUI_UPSTREAM.md)

Known renderer caveats (from GL3 comments): blur regions can affect hit-testing offsets; test tap targets on nav tabs after enabling glass.

## Performance budget

| Metric | Gate |
|--------|------|
| Frame time delta (compact chat scroll) | ≤ +2ms avg on M1 Mac; ≤ +4ms on mid Android (define device in LG005) |
| Backdrop samples | Minimize count — one shared glass layer per visible chrome bar, not per tab |
| Blur radius | Cap at 24px on low-tier; auto-disable via fallback |

Profiling commands: run compact chat with long thread; measure with platform GPU tools; compare glass on/off flag.

## Accessibility

| Requirement | Implementation |
|-------------|----------------|
| Reduce Transparency | Setting or system signal → `.surface-glass--opaque` (no backdrop-filter) |
| Reduce Motion | Disable scroll-linked blur/opacity animation |
| Contrast | Active tab + labels meet 4.5:1 on glass-fill-strong fallback |
| Touch targets | Preserve 44dp minimum ([UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md)) |

Wire to Me → Accessibility when that section exists, or reuse a profile pref.

## AI / agent constraints

When promoted (lg7):

- Update [RCSS_PROFILE.md](../../docs/ui/RCSS_PROFILE.md): glass utilities are **theme-only**, not for AI-generated RML.
- Update [PromptBuilder](../../src/base/ai/PromptBuilder.cpp) if needed to forbid `backdrop-filter` in model output.

## Test plan (summary)

| Case | Expect |
|------|--------|
| Compact Home — scroll chat under nav | Messages visible blurred through pill |
| Sessions — open thread overlay | Header glass over messages; back dismiss unchanged |
| Contacts transient | Glass header; `transient_back()` works |
| Auxiliary sheet | Sheet opens above nav; scrim unchanged |
| Dark / light theme | Tokens switch; no halo artifacts |
| Reduce transparency | Opaque chrome, no blur |
| Android rotation | GL reset restores blur ([PLATFORMS.md](../../docs/architecture/PLATFORMS.md)) |
| iOS safe area | Nav clears home indicator |

Automated: extend `shell_host_test` layout visibility only if needed; visual QA remains manual/screenshot.

## Promotion (when done)

Normative UI guidance → `docs/ui/UI_DESIGN_SYSTEM.md` (Materials section) and `docs/ui/WINDOW_SHELL.md` (compact chrome). Archive or mark this project **done** in [projects/README.md](../README.md).
