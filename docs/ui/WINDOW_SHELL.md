# Window Shell

**Tier:** product / UI

The Window Shell replaces the old split-panel layout with a role-based, responsive container for the chat UI.

## Subsystems

| Subsystem | Module | Role |
|-----------|--------|------|
| Layout | `ShellLayout` | Pure width/mode/visibility math |
| Navigation | `ShellHost`, `ViewCatalog`, `FlowCoordinator` | Pane registry, overlays, multi-step flows, DOM sync |
| Interruption | `ShellInterruption` | Escape/back/scrim dismiss ordering |
| Feedback | `ShellFeedback` | Toast, banner, alert/confirm dialog |

Shared state lives in `ShellState` (`ShellTypes.h`). Demos call shell APIs; they do not mount shell DOM directly.

## Pane roles

Each nav-rail tab is an independent **tab context**. Layout left-to-right:

```
Nav rail → Secondary (index/list) → Primary (drill-down) → Auxiliary → Transient
```

| Role | Position | Purpose | Expanded | Compact |
|------|----------|---------|----------|---------|
| Nav rail | Left / bottom | Tab switcher | Left column | Bottom bar |
| Secondary | First content column | Tab index / list (sessions, contacts) | Beside nav rail (Home tab omits this) | Full page above nav rail |
| Primary | Next column right | Tab drill-down content (home landing, chat, contact detail) | Center column when set; Home tab shows landing | Home: inline landing in nav page; Sessions: overlay on thread select |
| Auxiliary | Next column right | Further content in this tab (working set) | Right column when open | Sheet |
| Transient | Overlay | Deeper drill-down in this tab | Over primary | Over primary |

**Tab switch** (`SelectNavTab`) calls `ClearTabContext()`: primary pane cleared, auxiliary closed, transient stack cleared, compact overlay closed. Controllers mount tab-specific content into secondary and primary via `SetPrimaryPane(key)`.

Primary is **tab-scoped drill-down content**, not always chat. Examples:

| Tab | Secondary | Primary (when selected) |
|-----|-----------|-------------------------|
| Home (default) | (none) | Home landing (`home.rml`: brand, centered composer, suggestion chips) |
| Sessions | Session list | Chat + composer |
| Contacts | Contact list | Contact detail |

**Account / Me settings** are not a nav-rail tab. A profile button in the Home landing header opens an iOS-style **account bottom sheet** (`OpenAccountSheet` / `close_account_sheet`). Settings content mounts into `#pane-body-settings` inside the sheet. Swipe down on the grabber/header, or on the sheet body when scrollable ancestors are at the top, to dismiss (`ShellBottomSheetGesture`). A clean press/release (no dismiss past the drag deadzone) keeps normal click activation — e.g. preference disclosure rows open their section; a committed dismiss drag suppresses the following click. After a gesture dismiss, the sheet slides out then remounts home without requiring another input — `ShellHost` arms power-save via `NotifyFrameEnd` / `RequestNextUpdate` for the dismiss deadline. Tap the scrim or ×, or press Escape/back, to dismiss. Tab switch clears the sheet via `ClearTabContext()`.

The auxiliary pane is evolving from a reply mirror into a **working set** for browsable/actionable AI output (lists, forms, tables). See [WORKING_SET_PANEL.md](WORKING_SET_PANEL.md) for the implementation plan.

Layout mode switches at **768dp** width (`ShellConfig::compact_breakpoint_dp`).

## Interruption priority (high → low)

1. Dialog (alert/confirm)
2. Overlay layer (`push_layer`)
3. Transient stack
4. Account bottom sheet (Me / settings)
5. Auxiliary sheet (compact)
6. Compact chat overlay (compact)
7. Base panes

Escape (priority 110) calls `ShellHost::HandleDismiss()` before app quit (priority 100). Toasts and banners are informational and not in the dismiss stack.

## Presentation taxonomy

Choose the lightest primitive that fits the user task:

| Style | API | Use when | Examples |
|-------|-----|----------|----------|
| **Pane navigation** | `SetPrimaryPane`, `PushTransient` / `PopTransient` | User stays in a tab; back returns within that tab | Sessions→chat, Contacts→detail (compact uses transient) |
| **Account sheet** | `OpenAccountSheet` / `close_account_sheet` | Profile and preferences from Home without leaving the tab | Me settings, profile card, preference sections |
| **Modal flow** | `PushLayer` + `FlowCoordinator` | Task blocks the app until finished; may have multiple in-overlay steps | New conversation / group create (`PeoplePickerController`) |
| **Atomic feedback** | `ShellFeedback` dialog/toast | One-shot confirm/rename/prompt with no surrounding flow | Delete confirm, rename thread |

**Do not** stack `ShellFeedback::ShowPrompt` on top of an active `PushLayer` flow. Keep wizard steps in the same overlay (or push a dedicated step view on the overlay stack).

### FlowCoordinator

`FlowCoordinator` (`feature/ui/FlowCoordinator.*`) coordinates multi-step modal flows over overlay layers:

- `BeginModal(layer_id, on_step_back, on_cancel)` — register handlers when opening a modal flow
- `HandleDismiss()` — called from `ShellHost::HandleDismiss()` before popping overlays; step-back handler can return to a previous step instead of closing
- `NotifyLayerClosing(id)` — sync controller state when the user closes the layer via scrim/×

Reference implementation: group create in `PeoplePickerController` (select members → name group in one overlay).

### Compact drill-down

Prefer `PushTransient("contact_detail")` over inline `show_detail_` flags in list RML. The shell renders transient chrome (back button wired to `transient_back()`). Controllers should listen with `ShellHost::SetOnTransientPopped()` to clear selection state.

## RML / data model

Root document: `assets/samples/window_shell.rml` with `data-model="window"`.

| Callback | Action |
|----------|--------|
| `select_nav_tab(tab)` | Switch nav rail tab (`home`, `sessions`, or `contacts`); clears tab context |
| `open_account_sheet()` | Open Me / settings bottom sheet (Home landing profile button) |
| `close_account_sheet()` | Dismiss account bottom sheet |
| `compact_chat_back()` | Close compact chat overlay |
| `toggle_auxiliary()` | Open/close preview sheet/panel |
| `open_auxiliary()` | Open preview when available |
| `transient_back()` | Pop transient stack |
| `close_layer(id)` | Close overlay layer |
| `dismiss_banner()` | Hide banner |
| `dialog_ok()` / `dialog_cancel()` | Dialog buttons |

Nav rail badges bind to `window.nav_badges` (`sessions_unread`, `contacts_unread`, `me_attention`). The profile attention dot appears on the Home landing profile button, not the nav rail. Refreshed by `BadgeAggregator` on messaging events.

Pane bodies live in `assets/views/*.rml` and mount into `#pane-body-{key}`. The nav rail mounts from `assets/views/nav_rail.rml`.

## Home landing

Home is a dedicated primary pane (`home.rml`), not the chat panel:

| Region | Content |
|--------|---------|
| Top left | Brand wordmark (`app.name`) + tagline |
| Top right | Profile button (account sheet) |
| Optical center | Composer mounted into `#home-composer-mount` |
| Below composer | Soft suggestion chips (`send_suggestion`: find someone, headlines, articles, get started, capabilities) |

Sending from Home mints an AI thread, switches to Sessions, and opens chat (`EnsureHomeOutboundSession`). Compact Home mounts `#pane-body-home` inline in the nav page (no separate bottom composer slot).

## Composer chrome

Primary panes may set `provides_composer = true` on `PaneSpec`. The shell mounts `assets/views/composer.rml` into a dedicated slot instead of embedding the prompt inside pane scroll content. **Exception:** Home mounts the same composer body into `#home-composer-mount` inside the landing view so the prompt can sit centered with chips.

| Layout | Mount target | Structure |
|--------|--------------|-----------|
| Expanded (chat) | `#pane-composer-{key}` | Below `#pane-body-{key}` in the primary column |
| Expanded (home) | `#home-composer-mount` | Inside home landing stage |
| Compact | `#shell-composer-mount` | Sessions overlay only |
| Compact (home) | `#home-composer-mount` | Inside home landing; no bottom dock |

On compact, the composer appears on the Home landing (centered) or inside the Sessions chat overlay after selecting a thread (or after starting a chat from Home). Sessions and Contacts list pages do not show the composer.

## C++ usage

```cpp
ShellHost::Instance().RegisterPane({
    .key = "home",
    .rml_path = "views/home.rml",
    .role = PaneRole::Primary,
});
ShellHost::Instance().RegisterPane({
    .key = "chat",
    .rml_path = "views/chat.rml",
    .role = PaneRole::Primary,
    .provides_composer = true,
});
ShellHost::Instance().SelectNavTab(NavTab::Home);
ShellHost::Instance().SetPrimaryPane("home");
ShellHost::Instance().SyncLayout();

ShellFeedback::ShowBanner(ShellHost::Instance().State(), "Offline");
ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "Title", "Message", [](bool ok) { ... });
```

Call `ShellHost::Update(context)` each frame (resize, toast expiry).

## Compact floating chrome

When `layout_mode == compact`:

- **Bottom nav** floats above content (`position: absolute`; 12dp horizontal inset). Content panes use `padding-bottom` equal to nav height (56dp) plus safe-area bottom so the last row is not hidden.
- **Safe area** — top is edge-to-edge (body background under the status bar); `#shell-root` / chrome use `content_top_dp`. Bottom still insets the shell document. Values come from SDL safe-area insets and `machine.json` `safe_area` prefs (`ShellHost::RefreshSafeAreaInsets`).
- **Materials** — default opaque `.surface-chrome`; one bar may add `.surface-chrome--frost` (see [UI_DESIGN_SYSTEM.md](UI_DESIGN_SYSTEM.md#compact-floating-chrome-materials)). Reduce transparency (Me → Appearance) applies `.surface-chrome--solid` and disables frost.
- **Interruption order** unchanged — only presentation differs; chat overlay hides bottom nav while open.

Expanded layout keeps flat productivity chrome (no floating nav, no frost tier).

## Batches

- **Batch A:** Layout, navigation, Chat migration, DOM slots for feedback
- **Batch B:** Full interruption, feedback channels, confirm/banner in Chat
- **Batch C:** Activity strip, focus restore, docs
