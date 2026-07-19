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
| Secondary | First content column | Tab index / list (sessions, contacts, me) | Beside nav rail (Home tab omits this) | Full page above nav rail |
| Primary | Next column right | Tab drill-down content (chat, contact detail) | Center column when set; Home tab always shows chat | Home: inline chat in nav page; Sessions: overlay on thread select |
| Auxiliary | Next column right | Further content in this tab (working set) | Right column when open | Sheet |
| Transient | Overlay | Deeper drill-down in this tab | Over primary | Over primary |

**Tab switch** (`SelectNavTab`) calls `ClearTabContext()`: primary pane cleared, auxiliary closed, transient stack cleared, compact overlay closed. Controllers mount tab-specific content into secondary and primary via `SetPrimaryPane(key)`.

Primary is **tab-scoped drill-down content**, not always chat. Examples:

| Tab | Secondary | Primary (when selected) |
|-----|-----------|-------------------------|
| Home (default) | (none) | AI home chat + composer |
| Sessions | Session list | Chat + composer |
| Contacts | Contact list | Contact detail |
| Me | Compact identity strip + preference list | Preference section detail |

The auxiliary pane is evolving from a reply mirror into a **working set** for browsable/actionable AI output (lists, forms, tables). See [WORKING_SET_PANEL.md](WORKING_SET_PANEL.md) for the implementation plan.

Layout mode switches at **768dp** width (`ShellConfig::compact_breakpoint_dp`).

## Interruption priority (high → low)

1. Dialog (alert/confirm)
2. Overlay layer (`push_layer`)
3. Transient stack
4. Auxiliary sheet (compact)
5. Compact chat overlay (compact)
6. Base panes

Escape (priority 110) calls `ShellHost::HandleDismiss()` before app quit (priority 100). Toasts and banners are informational and not in the dismiss stack.

## Presentation taxonomy

Choose the lightest primitive that fits the user task:

| Style | API | Use when | Examples |
|-------|-----|----------|----------|
| **Pane navigation** | `SetPrimaryPane`, `PushTransient` / `PopTransient` | User stays in a tab; back returns within that tab | Sessions→chat, Contacts→detail (compact uses transient) |
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
| `select_nav_tab(tab)` | Switch nav rail tab (`home`, `sessions`, `contacts`, or `me`); clears tab context |
| `compact_chat_back()` | Close compact chat overlay |
| `toggle_auxiliary()` | Open/close preview sheet/panel |
| `open_auxiliary()` | Open preview when available |
| `transient_back()` | Pop transient stack |
| `close_layer(id)` | Close overlay layer |
| `dismiss_banner()` | Hide banner |
| `dialog_ok()` / `dialog_cancel()` | Dialog buttons |

Nav rail badges bind to `window.nav_badges` (`sessions_unread`, `contacts_unread`, `me_attention`). Refreshed by `BadgeAggregator` on messaging events.

Pane bodies live in `assets/views/*.rml` and mount into `#pane-body-{key}`. The nav rail mounts from `assets/views/nav_rail.rml`.

## Composer chrome

Primary panes may set `provides_composer = true` on `PaneSpec`. The shell mounts `assets/views/composer.rml` into a dedicated slot instead of embedding the prompt inside pane scroll content.

| Layout | Mount target | Structure |
|--------|--------------|-----------|
| Expanded | `#pane-composer-{key}` | Below `#pane-body-{key}` in the primary column |
| Compact | `#shell-composer-mount` | Home tab: below chat in nav page; Sessions overlay: inside overlay |

On compact, the composer appears on the Home tab (inline) or inside the Sessions chat overlay after selecting a thread. Me and other list pages do not show the composer.

## C++ usage

```cpp
ShellHost::Instance().RegisterPane({
    .key = "chat",
    .rml_path = "views/chat.rml",
    .role = PaneRole::Primary,
    .provides_composer = true,
});
ShellHost::Instance().SelectNavTab(NavTab::Home);
ShellHost::Instance().SetPrimaryPane("chat");
ShellHost::Instance().SyncLayout();

ShellFeedback::ShowBanner(ShellHost::Instance().State(), "Offline");
ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "Title", "Message", [](bool ok) { ... });
```

Call `ShellHost::Update(context)` each frame (resize, toast expiry).

## Batches

- **Batch A:** Layout, navigation, Chat migration, DOM slots for feedback
- **Batch B:** Full interruption, feedback channels, confirm/banner in Chat
- **Batch C:** Activity strip, focus restore, docs
