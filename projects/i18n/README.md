# Multi-language UI (i18n)

**Status:** i1–i6 landed (partial i5 coverage remains)  
**Owner:** agents + product  
**Stable refs:** [docs/ops/CONFIGURATION.md](../../docs/ops/CONFIGURATION.md), [docs/contracts/DATA_LAYOUT.md](../../docs/contracts/DATA_LAYOUT.md), [docs/ui/RML_PROFILE.md](../../docs/ui/RML_PROFILE.md)  
**Related:** Appearance preferences (`AppearanceSettingsSection`), shell/context menus (`ContextMenuHost`, `ShellHost`)

## One-line goal

Ship English and Simplified Chinese as first-class UI languages, selectable in Me → Appearance, with a catalog-and-key architecture that can add more locales without rewriting controllers or RML.

## Release scope (v1)

| In | Out |
|----|-----|
| `en` + `zh-Hans` locale catalogs | 100% string coverage on day one |
| Profile pref `language` (`system` / BCP-47) | Per-thread or per-contact language |
| Settings picker: float desktop, bottom sheet compact/mobile | OS translation of AI chat content |
| Runtime language switch without restart | RTL layout (Arabic/Hebrew) |
| CJK fallback font subset | ICU / gettext tooling |
| `Tr(key)` + `{name}` interpolation + `{{i18n:key}}` RML tokens | Full MessageFormat plurals |

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Architecture, catalog format, UI flow, string ownership |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PHASES.md](PHASES.md) | Delivery checklist |
| [DECISIONS.md](DECISIONS.md) | ADRs (I001+) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| i0 | Project docs + ADRs | Done |
| i1 | Pref + LocalizationService + catalogs | Done |
| i2 | Settings language picker (sheet on mobile) | Done |
| i3 | Localize shell chrome + settings + nav | Done |
| i4 | Fonts / shaping for zh-Hans | Done (rmlui-language prop deferred) |
| i5 | Remaining feature surfaces | Partial — continue widening |
| i6 | Promote contracts + ops docs | Done |
