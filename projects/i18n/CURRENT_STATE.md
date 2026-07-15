# Multi-language UI — current state

**As of:** 2026-07-15 (planning only; no implementation)

## What exists

| Area | State |
|------|-------|
| App-level i18n | **None** — no catalogs, `Tr()`, locale pref, or language settings |
| User-facing strings | Hardcoded English in RML assets and C++ |
| Settings sections | Profile, Assistant, Integrations, Network, Security, Appearance, Storage |
| Appearance | Theme only (`system` / `light` / `dark`) → `preferences.json` |
| Profile prefs schema | `ProfilePreferences::kSchemaVersion = 5` |
| Dropdowns | RmlUi `<select>` used for Theme and other settings fields |
| Mobile bottom sheets | `ContextMenuHost::ShowActions` → action sheet on compact layout |
| Fonts | Single face: `assets/fonts/LatoLatin-Regular.ttf` (Latin-oriented) |
| RmlUi language property | Fork supports `--rmlui-language` for shaping; app does not set it |
| OS locale | SDL has locale APIs in third_party; app does not call them |

## String hotspots (not exhaustive)

- `assets/views/{nav_rail,settings,settings_detail,chat,contacts,contact_detail,sidebar,composer}.rml`
- `src/feature/settings/*SettingsSection.cpp` (`ListItem` titles/subtitles)
- `src/feature/ui/{SettingsController,ShellHost,PinGateController,ContactsController}.cpp`
- `src/feature/chat/ChatController.cpp`
- `src/base/ui/ContextMenuHost.cpp` (`Cancel`)
- `src/base/error/AppError.cpp`
- `src/base/ai/{PromptBuilder,StructuredTextParser}.cpp`

## Gaps vs design

1. No `language` on `ProfilePreferences`.
2. No `LocalizationService` / `assets/locales/`.
3. No language listener on `SessionStore`.
4. No CJK fallback font.
5. No selected-state checkmarks on context-menu action sheets (needed for language picker polish).
6. Settings detail markup duplicated in compact + expanded RML.

## Next agent — start here

1. Read [DESIGN.md](DESIGN.md) and [DECISIONS.md](DECISIONS.md).
2. Implement phase **i1** in [PHASES.md](PHASES.md): pref + service + `en` / `zh-Hans` catalogs with a minimal key set.
3. Then **i2** picker in Appearance, then **i3** chrome localization.
