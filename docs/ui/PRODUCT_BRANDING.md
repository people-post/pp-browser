# Product branding — Frame

**Tier:** product / UI  
**Status:** adopted (2026-07-17)  
**PR:** [#28](https://github.com/people-post/pp-browser/pull/28)

User-facing product identity for the AI-centric browsing shell. Repo codename remains **`pp-browser`** for CMake targets, config paths, and wire protocol IDs.

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

Constants live in [`src/base/platform/ProductBranding.h`](../../src/base/platform/ProductBranding.h):

```cpp
kProductName    = "Frame"
kProductTagline = "The internet, rendered for you."
kProductSlug    = "frame"   // bundle id, CPack, future slug migrations
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
| SDL window title | Frame |
| Linux `notify-send --app-name` | Frame |
| HTTP User-Agent (web search tool) | `Frame/0.1 …` |
| Locales `app.name` / `app.tagline` | Frame + tagline (EN, zh-Hans) |
| Chat empty state | Product name + tagline + existing `chat.empty_body` |
| macOS bundle (packaged) | `Frame.app`, icon `app-icon.png`, id `dev.frame.app` |
| Windows CPack / NSIS | Display name Frame; embedded `app-icon.ico` |
| README | Frame as product name; pp-browser as repo codename |

**Runtime icon:** [`WindowIcon`](../../src/base/platform/WindowIcon.cpp) loads `branding/app-icon.png` via SDL after window creation (desktop only).

---

## Intentionally unchanged (compatibility)

These stay **`pp-browser`** until a deliberate migration:

| Area | Reason |
|------|--------|
| CMake target `pp-browser` | Scripts, CI, and docs reference it |
| Config/data dirs (`~/.config/pp-browser`, etc.) | Avoid breaking existing profiles |
| P2P protocol IDs (`/pp-browser/chat/1.0.0`, …) | Wire compatibility with peers |
| Crypto bundle strings (`pp-browser-psk-bundle-v1`) | On-disk / clipboard format |
| GitHub repo name | Org/process change, not branding alone |

A future **slug migration** (`frame` data paths, executable rename) should be a separate ADR with wipe-vs-migrate policy per [`COMPATIBILITY.md`](../contracts/COMPATIBILITY.md).

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
- [ ] Decide whether to rename executable and data dirs for 1.0
- [ ] Generate `.icns` for macOS if packaged DMG quality bar requires it
