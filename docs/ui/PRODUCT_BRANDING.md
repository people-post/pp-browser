# Product branding — PP

**Tier:** product / UI  
**Status:** adopted

User-visible product identity for the AI-centric browsing shell.

## Naming rule (normative)

| Layer | Name | Examples |
|-------|------|----------|
| **User-visible** | **PP** | Window title, locales `app.name`, User-Agent, `CFBundleDisplayName` / `CFBundleName`, **`PP.app`** folder, DMG volume name, NSIS display/shortcuts labeled PP, Android launcher label |
| **Internal** | **pp-browser** | Repo, CMake target, config/data dirs, wire/protocol/crypto domains, reverse-DNS IDs, signing App IDs, AUMID, CPack package/file names (`pp-browser-<ver>-…`), log tags, process/binary name on desktop |

**`PP.app` vs identifiers:** the on-disk / Finder bundle folder is **`PP.app`**. Only `CFBundleIdentifier` and related signing IDs use the `pp-browser` scheme.

**Search tip:** `PP` is too short to grep reliably. Use `kProductName` / `PP_BROWSER_PRODUCT_NAME` for the marketing name; use `kProductSlug` / `pp-browser` for internal identity.

### Platform identifiers

| Platform | Identifier | Notes |
|----------|------------|--------|
| macOS | `dev.pp-browser.app` | `MACOSX_BUNDLE_GUI_IDENTIFIER`; Developer ID signing |
| iOS | `dev.pp-browser.ios` | `CFBundleIdentifier` |
| Windows AUMID | `dev.pp-browser.app` | Toast notifications |
| Android | `dev.pp_browser.app` | **Underscore required** — Android `applicationId` cannot contain hyphens |

### Constants (single source of truth)

C++ — [`src/foundation/runtime/ProductBranding.h`](../../src/foundation/runtime/ProductBranding.h):

```cpp
kProductName       = "PP"           // user-visible (marketing)
kProductBundleName = "PP"           // macOS/iOS .app folder + bundle executable
kProductSlug       = "pp-browser"   // internal repo / artifact slug
kProductLogTag     = "pp-browser"   // logcat / os_log / stderr dev prefix
kProductTagline    = "The internet, rendered for you."
kProductAumid      = "dev.pp-browser.app"
kAppIconAsset      = "branding/app-icon.png"
```

CMake — [`cmake/ProductBranding.cmake`](../../cmake/ProductBranding.cmake) (keep in sync):

- `PP_BROWSER_PRODUCT_NAME` → `kProductName`
- `PP_BROWSER_PRODUCT_SLUG` → `kProductSlug`

Plists under `packaging/*/Info.plist` hardcode **PP** for bundle display strings; update when `kProductName` changes.

Locales: `assets/locales/*/app.name` should match `kProductName`. Most UI binds `{{i18n:app.name}}`.

---

## Icon

**Asset:** [`assets/branding/app-icon.png`](../../assets/branding/app-icon.png) (1024² master; Windows uses [`app-icon.ico`](../../assets/branding/app-icon.ico)).

Mark: two stacked speech-bubble frames (sessions silhouette) in dual materials — matte teal in front, glossy sky-blue behind — on a light squircle.

Exploratory alternates live under [`design/branding/icon-candidates/`](../../design/branding/icon-candidates/) (outside `assets/`, not packaged).

**Runtime:** [`WindowIcon`](../../src/foundation/platform/WindowIcon.cpp) loads `branding/app-icon.png` via SDL after window creation (desktop). macOS/iOS bundles embed the same PNG.

---

## Wiring

| Surface | Value |
|---------|--------|
| SDL window title | `kProductName` (PP) |
| Desktop in-app title bar | PP (`i18n:app.name` in `#shell-titlebar`) |
| Linux `notify-send --app-name` | PP |
| HTTP User-Agent (web search tool) | `PP/<version> …` |
| Locales `app.name` / `app.tagline` | PP + tagline (EN, zh-Hans) |
| macOS bundle (packaged) | `PP.app`, id `dev.pp-browser.app` |
| iOS bundle | `PP.app`, display name PP, id `dev.pp-browser.ios` |
| Windows CPack / NSIS | Display name PP; artifact `pp-browser-<ver>-windows-x64.exe` |
| macOS CPack DMG | Volume name PP; artifact `pp-browser-<ver>-macos.dmg` |
| Android launcher | PP (`strings.xml`); id `dev.pp_browser.app` |
| Dev logs | `kProductLogTag` (`pp-browser`) — `adb logcat -s pp-browser:W` |
| README | PP as product name; pp-browser as repo / internal slug |

### Advanced / technical copy (keep pp-browser)

| Surface | Example |
|---------|---------|
| Firewall help | “Allow PP (pp-browser) in your OS firewall…” |
| Wire / paste formats | `pp-browser-psk-bundle-v1`, `pp-browser-link-device-v1` |
| Process name (Task Manager / `ps`) | `pp-browser` / `pp-browser.exe` |
| Config paths | `~/.config/pp-browser`, … |

---

## Internal surfaces (already `pp-browser`)

| Area | Notes |
|------|--------|
| CMake target `pp-browser` | Scripts, CI, and docs reference it |
| Config/data dirs | See [DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md) |
| P2P protocol IDs (`/pp-browser/chat/1.0.0`, …) | Wire compatibility with peers |
| Crypto / signing domains | On-disk / clipboard / API |
| GitHub repo name | Org/process; not a product rename |
| Release artifact filenames | `pp-browser-<version>-…` |
| Entitlements files | `packaging/*/pp-browser.entitlements` |
