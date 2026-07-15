# Multi-language UI — phases

Ordering only. Prefer thin vertical slices that users can switch language against, then widen coverage.

## i0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`

## i1 — Preference + LocalizationService + catalogs

- [ ] `ProfilePreferences::language` default `"system"`; bump schema 5 → 6
- [ ] Serialize/deserialize in `ConfigJson`; update defaults / tests
- [ ] `src/base/i18n/LocalizationService.*` (+ unit tests)
- [ ] `assets/locales/en.json` and `assets/locales/zh-Hans.json` with **minimal** key set (common + nav + appearance/language + settings.saved)
- [ ] Resolve `system` via OS preferred locales (SDL); fallback `en`
- [ ] `SessionStore::AddLanguageListener` / notify on resolved-language change
- [ ] Bootstrap / Application loads catalogs and applies profile language at startup

## i2 — Settings language picker

- [ ] `SettingsUiState::language` (+ display label for current selection)
- [ ] Extend `AppearanceSettingsSection` to sync/flush `language` with appearance (or dedicated flush helper)
- [ ] Appearance detail UI: Language row in `settings.rml` **and** `settings_detail.rml`
- [ ] Tap opens picker via `ContextMenuHost::ShowActions`
- [ ] Compact layout: bottom action sheet; expanded: floating list
- [ ] Optional `ContextMenuAction::selected` checkmark for active language
- [ ] Immediate save + language listener refreshes UI
- [ ] Settings section tests for language flush/reset

## i3 — Localize shell chrome + settings + nav

- [ ] Section `ListItem()` titles/subtitles via `Tr()`
- [ ] Nav rail labels via bindings or i18n tokens
- [ ] Settings detail field labels / option labels / helper text for Appearance (+ other sections already showing)
- [ ] Shell dialog Cancel/OK via `Tr()` at RML serialize time
- [ ] Context menu sheet Cancel via `Tr()`
- [ ] Settings toast “Settings saved” via `Tr()`
- [ ] On language change: dirty models and/or remount active panes without full process restart

## i4 — Fonts / shaping for zh-Hans

- [ ] License-compatible CJK fallback font under `assets/fonts/`
- [ ] `LoadFontFace(..., fallback_face=true)` in Application startup
- [ ] Set `--rmlui-language` (and document where) when resolved locale changes
- [ ] Manual smoke: Chinese nav + settings + toast render without tofu

## i5 — Remaining feature surfaces

Priority order (can split PRs):

1. PIN gate (`PinGateController` + shell PIN chrome)
2. Contacts list/detail + menus/toasts
3. Chat chrome (status, empty state, context menus, confirms) — **not** model output
4. `AppError` / shared error strings
5. Sidebar / composer placeholders
6. Optional: structured-widget chrome (`StructuredTextParser` weekdays, etc.)

- [ ] Each surface uses `Tr()` or RML tokens; no new English literals for user-visible chrome
- [ ] Catalog keys added to both `en` and `zh-Hans`
- [ ] Optional CI check: `zh-Hans` covers all `en` keys

## i6 — Promote to stable docs

- [ ] DATA_LAYOUT: `language` on `preferences.json`, schema version
- [ ] CONFIGURATION: language preference + System behavior
- [ ] COMPATIBILITY note if newer prefs schema matters for dirty disk
- [ ] Refresh CURRENT_STATE / README status
- [ ] ADR freeze notes → superseded by docs where appropriate

## Out of scope follow-ups (track elsewhere if needed)

- Assistant reply language / `PromptBuilder` locale (ADR I005)
- Traditional Chinese, Japanese, Korean catalogs
- RTL layout project
- Extracted reusable `PickerSheet` component
- Translation contributor workflow / external TMS
