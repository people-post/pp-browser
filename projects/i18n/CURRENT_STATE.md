# Multi-language UI — current state

**As of:** 2026-07-15 (implementation landed through i6; residual string coverage remains)

## What exists

| Area | State |
|------|-------|
| App-level i18n | **Yes** — `LocalizationService` + `assets/locales/{en,zh-Hans}.json` |
| Preference | `ProfilePreferences::language` (`system` / BCP-47), schema v6 |
| Settings UI | Me → Appearance → Language row; bottom sheet (compact) / float list (expanded) via `ContextMenuHost` |
| Apply | `SessionStore` language listener → set catalog + remount shell + refresh settings chrome |
| Fonts | `LatoLatin` + `NotoSansSC-Regular.subset.ttf` fallback |
| RML tokens | `{{i18n:key}}` replaced in `ViewCatalog::LoadBody` |
| Tests | `pp_browser_localization_service_test`; settings sections load catalogs |

## Localized surfaces (v1 ship)

- Nav rail labels
- Settings section titles/subtitles + Appearance theme/language + many settings field labels
- Shell dialog Cancel/OK; PIN gate chrome buttons/placeholders + controller titles
- Context menu Cancel + selected checkmark
- AppError catalog messages
- Chat delete / clear history titles; composer placeholder (shared key)
- Contacts add menu / remove confirm title

## Still English (follow-up)

- Many chat status / transport strings and empty-state RML copy
- Contacts detail body copy, trust labels, assorted toasts
- Settings Profile registration status strings from net util
- Structured chat widget chrome / LLM prompts (out of UI language scope per I005)

## Next agent — start here

1. Widen i5 coverage: chat empty RML, contacts body, remaining toasts/menus.
2. Optional CI check that `zh-Hans.json` covers every `en.json` key.
3. Consider extracting a reusable `PickerSheet` if more pickers appear.
