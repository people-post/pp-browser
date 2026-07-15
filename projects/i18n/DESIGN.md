# Multi-language UI — design

## Problem

All user-facing copy is English today: static RML, C++ literals (toasts, dialogs, section titles), and shell-generated markup. There is no locale preference, catalog, or runtime switch. Adding Chinese (and later more languages) requires a central string system before we localize surface by surface.

## Goals

1. **Selectable language** in Settings, with System / English / 简体中文 in v1.
2. **Scalable catalogs** — add a locale by dropping a JSON file + registering it; no controller rewrites.
3. **Hybrid string ownership** — C++ uses keys via `Tr()`; static RML migrates to bound or tokenized strings over phases.
4. **Instant apply** — changing language remounts/dirties UI like appearance changes today.
5. **Mobile UX** — language choice uses a bottom-up action sheet on compact layout (reuse / extend `ContextMenuHost`).

## Non-goals (v1)

- Translating peer message bodies or LLM replies automatically.
- Community-downloadable language packs at runtime.
- Full ICU MessageFormat / CLDR plural tables.
- RTL (LTR-only locales for v1: `en`, `zh-Hans`).

## Architecture

```
Settings (Appearance) → ProfilePreferences.language
        system | en | zh-Hans
                 │
                 │ SaveProfilePrefs
                 ▼
         SessionStore
         NotifyLanguageListeners(resolved_tag)
                 │
                 ▼
         LocalizationService (base/i18n)
         Load assets/locales/{tag}.json
         Fallback: requested → primary lang → en
         Tr("settings.appearance.title")
         Tr("errors.network_timeout", {seconds→30})
                 │
     ┌───────────┼───────────┐
     ▼           ▼           ▼
 Controllers  Data models  RML hydrate
 (toasts,     (section     (tokens /
  menus,       titles,      bound copy)
  dialogs)     nav labels)
```

### Layer placement

| Piece | Layer | Why |
|-------|-------|-----|
| Catalog load, `Tr`, locale resolve | `src/base/i18n/` | Shared by feature controllers; no UI deps |
| JSON catalogs | `assets/locales/` | Ship with assets; hot-reloadable in tests |
| Language settings section fields | `src/feature/settings/` | Matches Appearance / Profile prefs |
| Picker UI (row + sheet) | `SettingsController` + RML + `ContextMenuHost` | Feature UI |
| Language listener wiring | `SessionStore` + `Application` / `ShellHost` | Same pattern as appearance |

## Locale identity

Use **BCP-47 tags** as catalog file names and preference values:

| Tag | Display name (UI) | v1 |
|-----|-------------------|----|
| `system` | System | Pref only — not a catalog; resolves at runtime |
| `en` | English | Yes |
| `zh-Hans` | 简体中文 | Yes |
| `zh-Hant` | 繁體中文 | Later |
| `ja`, `ko`, … | … | Later |

**Resolution:**

1. If pref is `system` → first preferred OS locale that we ship a catalog for (SDL preferred locales); else `en`.
2. Exact tag match (`zh-Hans`) → that catalog.
3. Primary language match (`zh` → prefer `zh-Hans` if present).
4. Else `en`.

Store the **pref** as `system` / `en` / `zh-Hans`. Keep the **resolved** tag in memory for the running session.

## Catalog format

File: `assets/locales/en.json` (and `zh-Hans.json`).

```json
{
  "schema_version": 1,
  "locale": "en",
  "strings": {
    "common.cancel": "Cancel",
    "common.ok": "OK",
    "nav.home": "Home",
    "nav.sessions": "Sessions",
    "nav.contacts": "Contacts",
    "nav.me": "Me",
    "settings.appearance.title": "Appearance",
    "settings.appearance.subtitle": "Theme and language",
    "settings.language.label": "Language",
    "settings.language.system": "System",
    "settings.language.en": "English",
    "settings.language.zh_hans": "简体中文",
    "settings.saved": "Settings saved",
    "errors.network_timeout": "Request timed out after {seconds}s"
  }
}
```

**Rules:**

- Keys are stable dotted identifiers; never reuse a key with a different meaning.
- English catalog is the **source of truth** for key set. Missing keys in other locales fall back to English and should log once in debug builds.
- Interpolation: `{name}` only (simple replace). Nested braces / selects deferred.
- Keep `schema_version` so we can migrate catalog shape later.
- Do **not** embed HTML/RML markup in strings unless the key is documented as a raw RML fragment. Prefer composing structure in RML and translating leaf text.

Optional later: per-domain files (`en/settings.json`, …) merged at load — not required for v1.

## API sketch

```cpp
// src/base/i18n/LocalizationService.h
class LocalizationService {
public:
  static LocalizationService& Instance(); // or inject via Application

  Roe<void> LoadFromAssets(const std::string& assets_root);
  void SetPreferredLanguage(std::string pref); // system|BCP-47
  std::string PreferredLanguage() const;
  std::string ResolvedLanguage() const;

  std::string Tr(std::string_view key) const;
  std::string Tr(std::string_view key,
                 const std::map<std::string, std::string>& args) const;

  struct LocaleInfo { std::string tag; std::string native_name_key; };
  std::vector<LocaleInfo> AvailableLocales() const;
};
```

Convenience: `pbr::Tr("nav.home")` → `Instance().Tr(...)`.

Unit tests cover fallback chain, interpolation, and missing keys.

## Persistence

Extend `ProfilePreferences` (schema bump 5 → 6):

```json
{
  "schema_version": 6,
  "theme": "themes/base.rcss",
  "appearance": "system",
  "language": "system"
}
```

- Default: `"system"`.
- Unknown tags: fall back through resolve rules / allow-list; do not crash.
- Document in DATA_LAYOUT + CONFIGURATION when promoted (phase i6).

`SessionStore::AddLanguageListener` mirrors appearance: fire after successful `SaveProfilePrefs` when resolved language changes.

## Settings UI

### Placement

**Me → Appearance**, below Theme:

- Label: Language
- Control: current language display (not a long native `<select>` on mobile)
- Helper: short note that the app UI language changes immediately

Update Appearance list subtitle to mention language.

### Interaction

| Layout | Behavior |
|--------|----------|
| **Expanded (desktop)** | Same picker component as compact: floating list via `ContextMenuHost` Float presentation |
| **Compact (mobile)** | Bottom action sheet listing System / English / 简体中文, current selection marked |

**Preferred approach:** Language picker via `ContextMenuHost`.  
Tap Language row → `ShowActions` with one action per locale. Extend `ContextMenuAction` with optional `selected` (checkmark). Cancel remains the sheet dismiss control.

Do **not** rely solely on RmlUi `<select>` for language: it is not a bottom sheet on compact layout (user requirement).

Longer term, extract a reusable `PickerSheet` if more settings need the same pattern.

### Flush mode

Immediate (same as Appearance): change → update `SettingsUiState.language` → flush profile prefs → listeners → UI refresh.

## Applying a language change

On listener:

1. `LocalizationService::SetPreferredLanguage(pref)` → refresh resolved catalog if needed.
2. **Shell chrome:** regenerate or dirty bound strings (nav, dialogs, Cancel/OK, PIN chrome).
3. **Active panes:** remount bodies via existing mount path (`ViewCatalog` / `ShellHost` / `RmlMount`) **or** dirty all string bindings if fully data-driven.
4. Preserve selected settings section / scroll where `RmlMount` already preserves focus/scroll.
5. Controllers rebuild section list titles via `Tr()` on next sync.

v1 recommendation: **hybrid apply**

- Controllers already own many strings → call `Tr()` when building/dirtying.
- Static RML in first slice → either preprocess tokens at mount, or convert high-traffic labels to `data-rml` bindings from a `ui_strings` model.

### RML strategies (ordered)

| Strategy | When | Notes |
|----------|------|-------|
| **1. Data-bound strings** | Settings headers, nav, dynamic status | Fits DataModelHost; easy dirty-on-locale-change |
| **2. Mount-time token replace** | Dense static RML still literal | Distinct prefix such as `{{i18n:common.cancel}}` — must not collide with RmlUi `{{expr}}` in chat widgets |
| **3. Leave AI-generated content alone** | Chat bubbles from model | Accept as authored language |

Avoid inventing a parallel templating language beyond token replace.

## String ownership map

| Surface | Owner today | Localization approach |
|---------|-------------|------------------------|
| Nav rail | `nav_rail.rml` literals | Bind or tokens (i3) |
| Settings section titles | C++ `ListItem()` | `Tr()` in section handlers (i3) |
| Settings detail labels | Dual RML files | Bind/tokens; keep compact + expanded in sync (i3) |
| Shell dialogs Cancel/OK | `ShellHost` generated RML | `Tr()` at serialize time (i3) |
| Context menu Cancel | `ContextMenuHost` | `Tr()` (i3) |
| PIN gate | `PinGateController` + generated RML | `Tr()` (i5) |
| Chat status / menus | `ChatController` | `Tr()` (i5) |
| Contacts | `ContactsController` + RML | `Tr()` + tokens (i5) |
| Error catalog | `AppError.cpp` | Keys in catalog (i5) |
| Structured chat chrome | `StructuredTextParser` | Optional later; weekday labels etc. |
| LLM prompts | `PromptBuilder` | **Out of v1 UI scope** — see ADR I005 |

## Fonts and text shaping

Today only `LatoLatin-Regular.ttf` is loaded (`Application.cpp`). Chinese will tofu without a fallback.

v1 requirements:

1. Bundle a CJK-capable fallback font under `assets/fonts/` (license-compatible; confirm before commit).
2. `Rml::LoadFontFace(..., fallback_face=true)` for CJK.
3. Set RmlUi `--rmlui-language` on document/root when locale changes (`zh-Hans` / `en`) so shaping uses the right language tag.
4. Verify ellipsis/nowrap rows remain acceptable with localized labels.

## Testing

| Layer | Coverage |
|-------|----------|
| Unit | Catalog load, fallback, interpolation, system resolve with fake OS locales |
| Settings sections test | Language pref flush / reset to defaults |
| Manual | Switch EN↔ZH without restart; compact bottom sheet; System follows OS; relaunch preserves pref |
| Font smoke | Sample Chinese strings in nav + settings + toast |

## Adding a language later

1. Add `assets/locales/{tag}.json` (copy from `en.json`, translate values).
2. Register tag in `LocalizationService::AvailableLocales()` (or auto-scan assets).
3. Ensure the picker shows the language in its **native self-name** (ADR I003).
4. Ensure fonts cover the script.
5. No preference schema bump unless validation rules change.

## Risks

| Risk | Mitigation |
|------|------------|
| Dual `settings.rml` / `settings_detail.rml` drift | Shared binding keys; same labels; consider shared fragment later |
| Incomplete coverage looks half-translated | Phase order: chrome + settings first; ship picker with those surfaces |
| Key explosion / unused keys | Lint: every `en` key referenced or marked intentional; ZH completeness check in CI once ZH ships |
| Chat/AI English prompts vs ZH UI | ADR I005 — UI language only in v1 |
| Select vs sheet inconsistency | One picker path via ContextMenuHost |
