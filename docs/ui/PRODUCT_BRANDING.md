# Product branding — Frame

**Tier:** product / UI  
**Status:** adopted (2026-07-17); naming rule updated 2026-07-24  
**PR:** [#28](https://github.com/people-post/pp-browser/pull/28)

User-facing product identity for the AI-centric browsing shell.

## Naming rule (normative)

| Layer | Name | Examples |
|-------|------|----------|
| **User-visible** | **Frame** | Window title, locales `app.name`, User-Agent, `CFBundleDisplayName` / `CFBundleName`, **`Frame.app`** folder, DMG volume name, NSIS display/shortcuts labeled Frame |
| **Internal** | **pp-browser** | Repo, CMake target, config/data dirs, wire/protocol/crypto domains, reverse-DNS IDs, signing App IDs, AUMID, CPack package/file names (`pp-browser-<ver>-…`) |

**`Frame.app` vs identifiers:** the on-disk / Finder bundle folder stays **`Frame.app`**. Only `CFBundleIdentifier` and related signing IDs use the `pp-browser` scheme.

### Platform identifiers

| Platform | Identifier | Notes |
|----------|------------|--------|
| macOS | `dev.pp-browser.app` | `MACOSX_BUNDLE_GUI_IDENTIFIER`; Developer ID signing |
| iOS | `dev.pp-browser.ios` | `CFBundleIdentifier` |
| Windows AUMID | `dev.pp-browser.app` | Toast notifications |
| Android | `dev.pp_browser.app` | **Underscore required** — Android `applicationId` cannot contain hyphens; this is the platform spelling of the same logical `pp-browser` ID |

Pre-release: installs signed or registered as `dev.frame.*` are not migrated — re-register App IDs and reinstall.

Constants live in [`src/base/platform/ProductBranding.h`](../../src/base/platform/ProductBranding.h).

---

## Product context

Frame is an **AI-centric browsing tool**: users access the internet through intent and actions, not by rendering legacy websites or executing page JavaScript. The assistant talks to APIs and services; the app renders structured UI locally, shaped by the user's taste and settings.

Messaging, contacts, and related tools are **essential but secondary** — they satisfy social needs inside the same shell. Branding should communicate **intent-first access**, not “another browser tab” or “another chat app.”

---

## Icon — decision

### Chosen: portal viewport (soft sky palette)

**Asset:** [`assets/branding/app-icon.png`](../../assets/branding/app-icon.png)  
**Source mockup:** [`icon-mockup-08-soft-sky.png`](../../assets/branding/mockups/icon-mockup-08-soft-sky.png)

A soft portal/window frame containing clean geometric content blocks and a small action accent, with a thin line to an external node suggesting API/intent input.

**Why this direction wins**

| Criterion | Portal viewport |
|-----------|-----------------|
| Core pitch | Reads as “structured access, not a webpage” |
| Differentiation | Avoids globe, tab bar, and browser chrome clichés |
| Scalability | Strong silhouette at 16×16–1024×1024 |
| Social layer | External node can imply connectivity without chat-bubble dominance |

**Alternatives considered (saved under [`assets/branding/mockups/`](../../assets/branding/mockups/))**

| Mockup | Concept | When to prefer |
|--------|---------|----------------|
| `icon-mockup-01-portal-viewport.png` | Original dark slate + coral portal | Darker / warmer launch identity |
| `icon-mockup-02-compass-nodes.png` | Compass of connected nodes | More abstract brand; favicon-first identity |
| `icon-mockup-03-speech-viewport.png` | Speech bubble as viewport | Conversation is the primary entry metaphor |
| `icon-mockup-04-monogram-portal.png` | Letterform portal + action arrow | Monogram-first brand (pairs with a single-letter name) |
| `icon-mockup-05`–`07` | Light warm palettes (blush / sunny / berry) | Warmer “care” emotional brand |
| `icon-mockup-09-silver-cyan.png` | Silver gray + cyan accent | Cooler / more neon accent |
| `icon-mockup-10-slate-indigo.png` | Pale slate + indigo accent | Deeper blue, more solemn |

### Visual system (icon)

- **Shape:** rounded squircle with **transparent corners** (RGBA); one strong silhouette + one accent.
- **Palette:** cool light gray-blue base (`#F0F4F8`), white portal, soft slate content bars, sky-blue action accent (`#4A7CF0`) — aligns with UI `accent-primary`.
- **Style:** flat / soft; no glossy browser bezel or skeuomorphic chrome.
- **Density:** at 32×32, recognize portal + one accent only; social hints stay subtle.
- **Dock / launcher:** opaque light fills only inside the squircle; outside corners are alpha=0 so the icon blends with the desktop (do not ship an opaque square canvas).

### Clichés rejected

| Motif | Why skip |
|-------|----------|
| Globe + magnifying glass | Implies searching the old web |
| Robot face / generic AI sparkles | Indistinguishable “AI app” |
| Two speech bubbles | Reads as messaging-first |
| Browser tab bar | Anchors legacy browsing mental model |
| Circuit brain | Overused, ages quickly |

---

## Name — decision

### Chosen: **Frame**

**Tagline:** *The internet, rendered for you.*

**Why Frame**

- **Viewport metaphor** — pairs directly with the portal icon; what you see is a rendered frame, not a site.
- **Short and pronounceable** — works globally; easy in app stores and window titles.
- **Not “browser”** — avoids the Chrome / Safari / Arc category while staying honest about “seeing” the internet.
- **Room for AI + social** — neutral enough that messaging stays supporting infrastructure, not the headline.

```cpp
kProductName    = "Frame"                         // user-visible
kProductTagline = "The internet, rendered for you."
kProductSlug    = "pp-browser"                    // artifacts, ID helpers
kProductAumid   = "dev.pp-browser.app"
kAppIconAsset   = "branding/app-icon.png"
```

### Alternatives considered

Grouped by emphasis; none are wrong — Frame was selected for icon alignment and brevity.

**Intent & access:** Intent, Askway, Gate, Fetch, Relay  
**Render & taste:** Surface, Canvas, Palette, Loom  
**Navigate & act:** Helm, Scout, Pilot, Waypoint  
**Calm / premium:** Aperture, Prism, Meridian, Harbor, Atrium  
**Short / brandable:** Ava, Nox, Vela, Kova, Rune  

**Pairings that almost shipped**

| Icon | Name | Tagline sketch |
|------|------|----------------|
| Portal (#1) | **Frame** ✓ | The internet, rendered for you. |
| Portal (#1) | Surface | The internet, rendered for you. |
| Compass (#2) | Helm / Waypoint | Navigate by intent, not tabs. |
| Speech (#3) | Askway / Relay | Ask. Act. Connect. |

Before public launch, run **domain**, **trademark**, and **app-store name** checks on the final choice.

---

## Wiring (what changed)

| Surface | Value |
|---------|--------|
| SDL window title | Frame (task switcher / accessibility; window is borderless on desktop) |
| Desktop in-app title bar | Frame (`i18n:app.name` in `#shell-titlebar`) |
| Linux `notify-send --app-name` | Frame |
| HTTP User-Agent (web search tool) | `Frame/0.1 …` |
| Locales `app.name` / `app.tagline` | Frame + tagline (EN, zh-Hans) |
| Home landing | Product name + tagline (top-left brand block) |
| Chat empty state | Existing `chat.empty_body` (thread empty, no brand hero) |
| macOS bundle (packaged) | `Frame.app`, icon `app-icon.png`, id `dev.pp-browser.app` |
| iOS bundle | `Frame.app`, display name Frame, id `dev.pp-browser.ios` |
| Windows CPack / NSIS | Display name Frame; artifact `pp-browser-<ver>-windows-x64.exe`; embedded `app-icon.ico` |
| macOS CPack DMG | Volume name Frame; artifact `pp-browser-<ver>-macos.dmg` |
| README | Frame as product name; pp-browser as repo / internal slug |

**Runtime icon:** [`WindowIcon`](../../src/base/platform/WindowIcon.cpp) loads `branding/app-icon.png` via SDL after window creation (desktop only).

---

## Internal surfaces (already `pp-browser`)

| Area | Notes |
|------|--------|
| CMake target `pp-browser` | Scripts, CI, and docs reference it |
| Config/data dirs (`~/.config/pp-browser`, etc.) | See [DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md) |
| P2P protocol IDs (`/pp-browser/chat/1.0.0`, …) | Wire compatibility with peers |
| Crypto / signing domains (`pp-browser-psk-bundle-v1`, `pp-browser:relay-…`) | On-disk / clipboard / API |
| GitHub repo name | Org/process; not a product rename |
| Release artifact filenames | `pp-browser-<version>-…` (download names; installer UI still says Frame) |

Renaming the executable binary or GitHub repo is out of scope for branding alone.

---

## Asset inventory

```
assets/branding/
  app-icon.png          # Primary portal viewport (soft sky)
  app-icon.ico          # Windows executable icon
  app-icon.rc           # Windows resource script
  mockups/
    icon-mockup-01-portal-viewport.png   # original dark slate + coral
    icon-mockup-02-compass-nodes.png
    icon-mockup-03-speech-viewport.png
    icon-mockup-04-monogram-portal.png
    icon-mockup-05-soft-blush.png        # light warm: peach cream + rose-coral
    icon-mockup-06-sunny-care.png        # light warm: sunny cream + apricot
    icon-mockup-07-apricot-berry.png     # light warm: apricot blush + berry
    icon-mockup-08-soft-sky.png          # adopted: gray-blue + sky blue
    icon-mockup-09-silver-cyan.png       # light cool: silver gray + cyan
    icon-mockup-10-slate-indigo.png      # light cool: slate + indigo blue
```

---

## Review checklist

- [ ] Icon reads clearly at 16×16 and 32×32 (taskbar / dock)
- [ ] “Frame” distinct from existing apps/trademarks in target markets
- [ ] Tagline localized beyond EN / zh-Hans if needed
- [ ] Register Apple App IDs `dev.pp-browser.app` / `dev.pp-browser.ios` before signed builds
- [ ] Generate `.icns` for macOS if packaged DMG quality bar requires it
