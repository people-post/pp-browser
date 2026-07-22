# Floating Chrome — design

Compact mobile shell chrome: **floating geometry**, **opaque elevated surfaces**, and **optional single-surface backdrop frost** on the topmost visible chrome bar. Not Apple Liquid Glass parity.

**Pivot:** [LG008](DECISIONS.md#lg008--floating-chrome-top-layer-frost-only) (2026-07-22).

## Scope boundary

### In scope (compact shell chrome)

| Surface | RML / C++ | Chrome treatment |
|---------|-----------|-----------------|
| Bottom nav rail | `nav_rail.rml`, `.shell-nav-rail--compact` | Twin floating pills; opaque fill; frost when top chrome |
| Bottom chrome wrapper | `.shell-bottom-chrome` | Overlay positioning; safe-area padding |
| Chat overlay header | `.shell-chat-overlay-chrome` | Opaque bar; frost when chat overlay is top |
| Transient back header | `.shell-transient-chrome` | Opaque; frost when transient is top |
| Auxiliary sheet | `.shell-sheet-compact` | Opaque body; 8dp top strip; frost on strip when top |
| Account sheet header | `.shell-account-sheet-header` | Opaque sheet body; frost on header when top |
| Home composer chrome | `#shell-composer-mount` | Opaque strip (never frost) |

### Out of scope (v1)

- Expanded layout (≥768dp) nav column and pane dividers
- Chat bubbles, sidebar rows, settings lists, dialogs
- Multi-surface simultaneous `backdrop-filter`
- lg4 scroll-coupled motion; lg5 custom shaders
- Native platform materials outside RmlUi
- AI-generated arbitrary chrome widgets

## Visual principles

1. **Legibility first** — default chrome is ~94% opaque (`chrome-fill`); content behind is hinted via layout overlap, not required to read through bars.
2. **Floating hierarchy** — primary navigation inset from screen edges (horizontal margin + bottom safe area).
3. **Single frost cue** — when the interruption stack allows, one bar uses `blur(12px)` so scrolling content blurs through **one** control only.
4. **Specular edge** — 1dp highlight on top edge via border token; soft elevation shadow on pills/headers.
5. **Legibility** — icons and labels maintain contrast; active tab uses filled capsule.
6. **Static material** — no scroll-linked blur/opacity animation (lg4 cancelled).

## Layout architecture

Unchanged from lg1 — see [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md). Content extends under floating chrome; scroll padding preserves last-line visibility.

## Frost selection (normative)

`ShellInterruption::ResolveFrostSurface(state)` returns at most one target:

| Top interruption | Frost surface |
|------------------|---------------|
| `None` (base) | Bottom nav pills |
| `CompactChatOverlay` | Chat overlay header |
| `AuxiliarySheet` | Sheet top strip (`.shell-sheet-compact-chrome`) |
| `AccountSheet` | Account sheet header |
| `Transient` | Transient header |
| `Dialog`, `OverlayLayer`, `PinGate` | **None** (opaque modals) |

Expanded layout: always `None`.

## Material specification (RCSS tokens)

Add to `colors-light.rcss` / `colors-dark.rcss` inside theme blocks.

| Token | Light | Dark | Use |
|-------|-------|------|-----|
| `chrome-fill` | `#fffffff0` | `#1f2937f0` | Default opaque chrome |
| `chrome-fill-frost` | `#ffffffb3` | `#1f2937b3` | Fill when `--frost` active |
| `chrome-border-highlight` | `#ffffff66` | `#ffffff1a` | Top edge specular |
| `chrome-border-subtle` | `#00000014` | `#ffffff0f` | Separation |
| `chrome-blur-radius` | `12px` | `12px` | Frost tier only |
| `chrome-shadow` | box-shadow | box-shadow | Pill/header elevation |

Utility classes (theme-only):

```css
.surface-chrome {
  backdrop-filter: none;
  background-color: /* chrome-fill */;
  border-top: 1dp /* chrome-border-highlight */;
  border-radius: 22dp;
}
.surface-chrome--frost {
  backdrop-filter: blur(12px);
  background-color: /* chrome-fill-frost */;
}
.surface-chrome--solid {
  backdrop-filter: none;
  background-color: /* chrome-fill */;
}
```

## Renderer path

**Stock RCSS only.** No lg5 shader fork. Frost uses existing RmlUi `backdrop-filter: blur()` on one element.

## Performance budget

| Metric | Gate |
|--------|------|
| Backdrop-filter surfaces visible | **≤ 1** |
| Frame time delta (compact chat scroll) | ≤ +2ms avg vs all-opaque on reference device (LG005) |
| Blur radius | 12px frost tier; disabled entirely via `--solid` |

## Accessibility

| Requirement | Implementation |
|-------------|----------------|
| Reduce Transparency | Pref or system → `.surface-chrome--solid` on all chrome (no frost) |
| Reduce Motion | N/A (no motion tier) |
| Contrast | Active tab + labels on opaque chrome-fill |

## AI / agent constraints

When promoted (lg7): glass utilities documented as **theme-only** in [RCSS_PROFILE.md](../../docs/ui/RCSS_PROFILE.md).

## Test plan (summary)

| Case | Expect |
|------|--------|
| Compact Home — scroll chat under nav | Nav opaque; frost on nav only when base state |
| Open chat overlay | Frost moves to overlay header; nav hidden |
| Open auxiliary sheet | Sheet body opaque; frost on top strip only |
| Open transient over sheet | Frost on transient header only |
| Dialog open | No frost anywhere |
| Reduce transparency pref | All `--solid`, no blur |

Automated: `shell_host_test` covers `ResolveFrostSurface` mapping.
