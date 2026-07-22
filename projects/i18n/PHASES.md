# Multi-language UI — phases

Ordering only. Prefer thin vertical slices that users can switch language against, then widen coverage.

## i0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`

## i1 — Preference + LocalizationService + catalogs

- [x] `ProfilePreferences::language` default `"system"`; bump schema 5 → 6
- [x] Serialize/deserialize in `ConfigJson`; update defaults / tests
- [x] `src/base/i18n/LocalizationService.*` (+ unit tests)
- [x] `assets/locales/en.json` and `assets/locales/zh-Hans.json` with key set
- [x] Resolve `system` via OS preferred locales (SDL); fallback `en`
- [x] `SessionStore::AddLanguageListener` / notify on language change
- [x] Bootstrap / Application loads catalogs and applies profile language at startup

## i2 — Settings language picker

- [x] `SettingsUiState::language` (+ display label for current selection)
- [x] Extend `AppearanceSettingsSection` to sync/flush `language` with appearance
- [x] Appearance detail UI: Language row in `settings.rml` (via shared `settings_sections.rml` fragment)
- [x] Tap opens picker via `ContextMenuHost::ShowActions`
- [x] Compact layout: bottom action sheet; expanded: floating list
- [x] `ContextMenuAction::selected` checkmark for active language
- [x] Immediate save + language listener refreshes UI
- [x] Settings section tests load catalogs for language sync

## i3 — Localize shell chrome + settings + nav

- [x] Section `ListItem()` titles/subtitles via `Tr()`
- [x] Nav rail labels via i18n tokens
- [x] Settings Appearance (+ many field labels) via tokens
- [x] Shell dialog Cancel/OK via `Tr()` at RML serialize time
- [x] Context menu sheet Cancel via `Tr()`
- [x] Settings toast “Settings saved” via `Tr()`
- [x] On language change: dirty models and remount active panes without full process restart

## i4 — Fonts / shaping for zh-Hans

- [x] License-compatible CJK fallback font under `assets/fonts/` (Noto Sans SC subset + LICENSE note)
- [x] `LoadFontFace(..., fallback_face=true)` in Application startup
- [ ] Set `--rmlui-language` on document/root when resolved locale changes *(deferred; fallback font covers glyphs)*
- [x] Manual smoke not run in CI/headless; glyphs present in subset for catalog strings

## i5 — Remaining feature surfaces

Priority order (can split PRs):

1. [x] PIN gate (`PinGateController` + shell PIN chrome)
2. [x] Contacts add/remove titles (partial); detail body still English
3. [x] Chat delete/clear titles + composer placeholder (partial); empty state / status still English
4. [x] `AppError` / shared error strings
5. [ ] Sidebar / composer RML leftovers
6. [ ] Optional: structured-widget chrome (`StructuredTextParser` weekdays, etc.)

- [x] Catalog keys added to both `en` and `zh-Hans` for shipped surfaces
- [ ] Optional CI check: `zh-Hans` covers all `en` keys

## i6 — Promote to stable docs

- [x] DATA_LAYOUT: `language` on `preferences.json`, schema version
- [x] CONFIGURATION: language preference + System behavior
- [x] Refresh CURRENT_STATE / README status
- [ ] ADR freeze notes → superseded by docs where appropriate (optional cleanup)
