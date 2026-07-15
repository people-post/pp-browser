# Multi-language UI (i18n)

**Status:** i0 design — plan ready, implementation not started  
**Owner:** agents + product  
**Stable refs:** [docs/ops/CONFIGURATION.md](../../docs/ops/CONFIGURATION.md), [docs/contracts/DATA_LAYOUT.md](../../docs/contracts/DATA_LAYOUT.md), [docs/ui/RML_PROFILE.md](../../docs/ui/RML_PROFILE.md)  
**Related:** Appearance preferences (`AppearanceSettingsSection`), shell/context menus (`ContextMenuHost`, `ShellHost`)

## One-line goal

Ship English and Simplified Chinese as first-class UI languages, selectable in Me → Appearance, with a catalog-and-key architecture that can add more locales without rewriting controllers or RML.

## Release scope (v1)

| In | Out |
|----|-----|
| `en` + `zh-Hans` locale catalogs | Full UI coverage on day one (ship chrome + settings first) |
| Profile pref `language` (`system` / BCP-47) | Per-thread or per-contact language |
| Settings picker: dropdown desktop, bottom sheet compact/mobile | OS translation of AI chat content |
| Runtime language switch without restart | RTL layout (Arabic/Hebrew) |
| CJK fallback fonts | ICU / gettext tooling |
| `Tr(key)` + simple `{name}` interpolation | Plural/gender rules beyond ICU MessageFormat lite |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Architecture, catalog format, UI flow, string ownership |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today (no i18n) |
| [PHASES.md](PHASES.md) | Delivery checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (I001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| i0 | Project docs + ADRs | Done (this folder) |
| i1 | Pref + LocalizationService + catalogs | Not started |
| i2 | Settings language picker (sheet on mobile) | Not started |
| i3 | Localize shell chrome + settings + nav | Not started |
| i4 | Fonts / shaping for zh-Hans | Not started |
| i5 | Localize remaining feature surfaces | Not started |
| i6 | Promote contracts + ops docs | Not started |
