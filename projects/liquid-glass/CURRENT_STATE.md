# Floating Chrome — current state

**Last updated:** 2026-07-22  
**Status:** **Shipped** (lg1–lg3, lg8, lg6, lg7)

Normative guidance: [docs/ui/UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials), [docs/ui/WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md#compact-floating-chrome).

## Shipped

| Surface | Treatment |
|---------|-----------|
| Bottom nav | Twin floating pills; opaque `.surface-chrome`; frost when top chrome |
| Chat / transient headers | Opaque; frost when respective layer is top |
| Auxiliary sheet | Opaque body; frosted top strip when top |
| Account sheet | Opaque body; frosted header when top |
| Composer strip | Opaque (never frost) |

## Preferences (`preferences.json` schema v8)

| Field | Default | UI |
|-------|---------|-----|
| `reduce_transparency` | `false` | Me → Appearance → Reduce transparency |
| `compact_chrome_frost` | `true` | Profile JSON only (dogfood off switch) |

## Performance gate (LG005)

Reference devices for manual scroll profiling:

| Platform | Device |
|----------|--------|
| Android | Pixel 6a class (or equivalent mid-tier GLES) |
| iOS | iPhone 15 Simulator |

**Gate:** compact chat scroll avg frame time with frost ≤ +2ms vs all-opaque on reference Android; ≤ 1 backdrop-filter surface visible at any time.

## Rendering

| Capability | Status |
|------------|--------|
| `backdrop-filter` | ≤ 1 surface via `--frost` |
| Custom glass shaders | Cancelled (LG008) |
