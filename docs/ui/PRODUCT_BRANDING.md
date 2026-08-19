# Product branding — PP

**Tier:** product / UI  
**Status:** adopted (2026-07-17); product name **PP** (2026-08-19)  
**PR:** [#28](https://github.com/people-post/pp-browser/pull/28)

User-facing product identity for the AI-centric browsing shell.

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

Pre-release: installs signed or registered as `dev.frame.*` are not migrated — re-register App IDs and reinstall.

### Constants (single source of truth)

C++ — [`src/base/runtime/ProductBranding.h`](../../src/base/runtime/ProductBranding.h):

```cpp
kProductName       = "PP"           // user-visible (marketing)
kProductBundleName = "PP"           // macOS/iOS .app folder + bundle executable
kProductSlug       = "pp-browser"   // internal repo / artifact slug
kProductLogTag     = "pp-browser"   // logcat / os_log / stderr dev prefix
kProductTagline    = "The internet, rendered for you."
kProductAumid      = "dev.pp-browser.app"
```

CMake — [`cmake/ProductBranding.cmake`](../../cmake/ProductBranding.cmake) (keep in sync):

- `PP_BROWSER_PRODUCT_NAME` → `kProductName`
- `PP_BROWSER_PRODUCT_SLUG` → `kProductSlug`

Plists under `packaging/*/Info.plist` hardcode **PP** for bundle display strings; update when `kProductName` changes.

Locales: `assets/locales/*/app.name` should match `kProductName`. Most UI binds `{{i18n:app.name}}`.

---

## Product context

PP is an **AI-centric browsing tool**: users access the internet through intent and actions, not by rendering legacy websites or executing page JavaScript. The assistant talks to APIs and services; the app renders structured UI locally, shaped by the user's taste and settings.

Messaging, contacts, and related tools are **essential but secondary** — they satisfy social needs inside the same shell.

---

## Icon — decision

**Asset:** [`assets/branding/app-icon.png`](../../assets/branding/app-icon.png)  
**Source mockup:** [`icon-mockup-11-balanced-sky.png`](../../assets/branding/mockups/icon-mockup-11-balanced-sky.png)

A soft portal/window frame containing clean geometric content blocks and a small action accent, with a thin line to an external node suggesting API/intent input. Mockup history: [`assets/branding/mockups/`](../../assets/branding/mockups/).

### Visual system (icon)

- **Shape:** rounded squircle with **hard transparent margin + corners** (RGBA); ~10% transparent padding per side on the 1024² master.
- **Palette:** cool slate plate (`#BECCDE`), white portal frame, `#4a6cf7` accent — matches UI `accent-primary`.
- **Style:** flat / soft; depth only inside the squircle.

---

## Name — decision

### Chosen: **PP**

**Tagline:** *The internet, rendered for you.*

Short, pronounceable, aligned with People Post / pp-browser lineage. Not “browser” — avoids the Chrome / Safari category.

**Previous user-visible name:** Frame (2026-07-17 → 2026-08-19). Internal slug unchanged.

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

**Runtime icon:** [`WindowIcon`](../../src/base/platform/WindowIcon.cpp) loads `branding/app-icon.png` via SDL after window creation (desktop only).

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

Renaming the executable binary or GitHub repo is out of scope for branding alone.

---

## Asset inventory

```
assets/branding/
  app-icon.png          # Primary portal viewport (balanced soft sky)
  app-icon.ico          # Windows executable icon
  app-icon.rc           # Windows resource script
  mockups/              # Icon exploration history
```

---

## Review checklist

- [ ] Icon reads clearly at 16×16 and 32×32 (taskbar / dock)
- [ ] “PP” distinct from existing apps/trademarks in target markets
- [ ] Tagline localized beyond EN / zh-Hans if needed
- [ ] Apple App IDs `dev.pp-browser.app` / `dev.pp-browser.ios` registered before signed builds
- [ ] iOS provisioning profile renamed to “pp-browser iOS Development” in Apple Developer account
