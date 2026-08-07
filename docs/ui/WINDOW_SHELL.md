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
| Me | Settings list + profile card | Section detail (`settings_detail.rml`) |

**Me / account settings** use a **layout-adaptive** presentation:

| Layout | Entry | Presentation |
|--------|--------|----------------|
| **Expanded** | Nav rail Me tab (`select_nav_tab('me')`) | Secondary list → primary section detail (same pattern as Contacts) |
| **Compact** | Home header profile button (`open_account_sheet`) | Existing **account bottom sheet** over the current tab. List→detail stays inside the sheet with local back / swipe dismiss. |

Resizing between compact and expanded migrates Me between sheet and tab when appropriate (`SettingsController::SyncLayoutMode`). Tab switch still clears an open account sheet via `ClearTabContext()`.

The auxiliary pane is evolving from a reply mirror into a **working set** for browsable/actionable AI output (lists, forms, tables). See [WORKING_SET_PANEL.md](WORKING_SET_PANEL.md) for the implementation plan.

Layout mode switches at **768dp** width (`ShellConfig::compact_breakpoint_dp`).

**Team UI review mock:** open [`shell_layout_review.html`](shell_layout_review.html) in a browser for side-by-side expanded vs compact page compositions (static HTML facsimile of the RML shell — not a live render).

## Interruption priority (high → low)

1. Dialog (alert/confirm)
2. Overlay layer (`push_layer`)
3. Transient stack
4. Account bottom sheet (Me / settings)
5. Auxiliary sheet (compact)
6. Compact chat overlay (compact)
7. Base panes

Compact z-index must match this order so sheets are not painted under the chat overlay: chat `25`, auxiliary scrim/sheet `26`/`27`, account scrim/sheet `28`/`29`, transient `30` (see `assets/themes/components.rcss`).

Escape (priority 110) calls `ShellHost::HandleDismiss()` before app quit (priority 100). Toasts and banners are informational and not in the dismiss stack.

## Presentation taxonomy

Choose the lightest primitive that fits the user task:

| Style | API | Use when | Examples |
|-------|-----|----------|----------|
| **Pane navigation** | `SetPrimaryPane`, `PushTransient` / `PopTransient` | User stays in a tab; back returns within that tab | Sessions→chat, Contacts→detail (compact uses transient) |
| **Account sheet** | `OpenAccountSheet` / `close_account_sheet` | Compact Me: profile and preferences without switching `nav_tab` | Me settings list/detail inside sheet |
| **Modal flow** | `PushLayer` + `FlowCoordinator` | Task blocks the app until finished; may have multiple in-overlay steps | New conversation / group create (`PeoplePickerController`) |
| **Atomic feedback** | `ShellFeedback` dialog/toast | One-shot confirm/rename/prompt with no surrounding flow | Delete confirm, rename thread |

**Do not** stack `ShellFeedback::ShowPrompt` on top of an active `PushLayer` flow. Keep wizard steps in the same overlay (or push a dedicated step view on the overlay stack).

### DOM sync: DirtyWindow vs SyncLayout

`SyncLayout` remounts the shell DOM (`SerializeShellRoot` + pane bodies). Use it only for **structural** changes. Binding updates must not remount. Prefer domain dirty helpers over grab-bag `DirtyWindow()`.

| Need | API |
|------|-----|
| Nav / badges / sheet / auxiliary bindings | `DirtyNavChrome()` |
| Banner / toast / dialog bindings | `DirtyFeedback()` |
| PIN gate / unlock_in_progress | `DirtyPinGate()` |
| Activity / statusbar / titlebar / fonts | `DirtyStatusChrome()` |
| Full refresh after shell remount | `DirtyWindow()` (calls all domains; used by `SyncLayout`) |
| Shell tree change (nav, panes, overlays, dialog, layout mode) | `RequestSyncLayout(reason)` |
| Call chrome labels / icons / meters (layer already mounted) | `apply_chrome_update(DirtyOnly)` → `DirtyCallChrome()` |
| Call ring / in-call layer appear or disappear | `apply_chrome_update(Remount)` → `RemountCallChrome()` (not full `SyncLayout`) |
| Periodic poll / tick | Reconcile state only; remount **iff** structure changed; dirty only when bindings changed |

Do **not** pair `RequestSyncLayout` with an extra domain dirty — `SyncLayout` already calls `DirtyWindow()`.

### Surface vs shell projection

Presenters push a **surface snapshot** upward (`*SurfaceNotifyPorts`). An **app-owned bridge** (`ContactsShellBridge` / `ChatShellBridge` / `PeoplePickerShellBridge`) knows both the snapshot and `ShellHost`: it projects → classifies → applies `DirtyNav` / `SyncLayout` via `ShellChromeApplyPorts`. Controllers must not call grab-bag nav dirty. Toast/banner use `ShellFeedbackPorts` (already `DirtyFeedback`).

Call chrome stays special-cased (`CallController` + `CallChromeSync`). See [SHELL_CHROME_PROJECTION.md](../architecture/SHELL_CHROME_PROJECTION.md).

Call ring / in-call overlays live in `#shell-call-ring-mount` / `#shell-call-in-progress-mount`. `CallController` classifies changes and notifies ShellHost via `apply_chrome_update` — it does **not** call grab-bag `DirtyWindow`. Show/hide remounts **only those mounts** via `RemountCallChrome` — never remount the full shell tree for call chrome (that destroyed chat panes and broke Accept hit-testing on Samsung). Control *presence* (stage / retry / invite / speaker button / roster) and **chrome mode** (Expanded / Immersive / Minimized — V031) also remount. Mute / speaker / camera icon toggles use `DirtyCallChrome` + single SVG `data-attr-src` (and `--on` class) — not remount/bake, and not dual `data-if` SVGs. Meter/pulse/elapsed ticks use `DirtyCallChrome` only. After each in-call remount, `ShellCallChromeGesture` re-attaches to `#shell-call-chrome-root` (or the minimized chip).

`RemountCallChrome` **defers** `MountInner` to the next UI turn (same reason as deferred `SyncLayout`): doing it inside an Rml Leave/Accept click destroys the event target mid-dispatch and can segfault. `SyncLayout` still fills mounts synchronously after the shell remount.

**Why not always-mounted `data-if` for the Accept layer?** Binding state can be true and the frame loop can Present while Rml `data-if` leaves the overlay at `display:none`. Dialogs already use presence-based mount (`SerializeDialog` empty vs HTML). Call chrome follows that pattern scoped to dedicated mounts so chat panes stay intact. Inner `data-if` (conflict copy, video stage/PiP) remains fine for fields *inside* an already-mounted layer.

Dialog open/close remount is owned by `ShellFeedback` (`dialog_open` / `dialog_close`). Callers of `ShowConfirm*` / `ShowAlert` / `ShowPrompt` must not also call `RequestSyncLayout` solely to show the dialog.

Timers (e.g. foreground relay poll) must never remount “just in case.”

### FlowCoordinator

`FlowCoordinator` (`feature/ui/FlowCoordinator.*`) coordinates multi-step modal flows over overlay layers:

- `BeginModal(layer_id, on_step_back, on_cancel)` — register handlers when opening a modal flow
- `HandleDismiss()` — called from `ShellHost::HandleDismiss()` before popping overlays; step-back handler can return to a previous step instead of closing
- `NotifyLayerClosing(id)` — sync controller state when the user closes the layer via scrim/×

Reference implementation: group create in `PeoplePickerController` (select members → name group in one overlay).

### Compact drill-down

Prefer `PushTransient("contact_detail")` over inline `show_detail_` flags in list RML. The shell renders transient chrome (back button wired to `transient_back()`). Controllers should listen with `ShellHost::SetOnTransientPopped()` to clear selection state.

### Dismiss pipeline

Escape, chrome back buttons, and swipe gestures share `ShellHost::RequestDismiss(style, force?)`:

| Input | Typical target |
|-------|----------------|
| Escape / system back | Top of local-back stack, else `ShellInterruption::Top` |
| `transient_back` / compact chat back | Forced transient / chat overlay |
| Horizontal edge swipe | Same as back for that surface (`ShellSwipeBackGesture`) |
| Vertical sheet swipe | Forced `AccountSheet` (still works over settings detail) |
| Account sheet × | `CloseAccountSheet()` (clears nested local back) |

Nested list→detail inside a sheet (Me settings) uses `PushLocalBack("settings_detail", commit)` so Escape/swipe-back pop detail before dismissing the sheet. Swipe-back and sheet dismiss may both arm; `ShellGestureAxisLock` commits the dominant axis after the deadzone (edge horizontal → back; vertical from anywhere at scroll top → dismiss sheet).

## RML / data model

Root document: `assets/samples/window_shell.rml` with `data-model="window"`.

| Callback | Action |
|----------|--------|
| `select_nav_tab(tab)` | Switch nav rail tab (`home`, `sessions`, `contacts`, or `me` on expanded); clears tab context. On compact, `me` still opens the account sheet if invoked. |
| `open_account_sheet()` | Open Me / settings bottom sheet (compact Home profile button) |
| `close_account_sheet()` | Dismiss account bottom sheet |
| `compact_chat_back()` | Close compact chat overlay |
| `toggle_auxiliary()` | Open/close preview sheet/panel |
| `open_auxiliary()` | Open preview when available |
| `transient_back()` | Pop transient stack |
| `close_layer(id)` | Close overlay layer |
| `dismiss_banner()` | Hide banner |
| `dialog_ok()` / `dialog_cancel()` | Dialog buttons |
| `titlebar_minimize()` / `titlebar_toggle_maximize()` / `titlebar_close()` | Desktop custom title bar window controls (macOS traffic lights or Win/Linux icon strip) |

Desktop expanded status bar binds `statusbar_visible`, cluster fields (`statusbar_brief_*`, `statusbar_direct_*`, `statusbar_help_visible`, `statusbar_inbound_*`, `statusbar_label*`), and `statusbar_activity` (no click callbacks — display-only). Product work: [network-status-chrome](../../projects/network-status-chrome/).

Nav rail badges bind to `window.nav_badges` (`sessions_unread`, `contacts_unread`, `me_attention`). `sessions_unread` is aggregate chat unread; `contacts_unread` is reserved for future contacts-tab queues (not chat unread — currently always 0). `me_attention` is a non-count dot for Me setup nudges (today: desktop Node outbound-only/blocked reachability until the user acks **Got it — use relay** in Me → Network, or inbound becomes reachable). On expanded, the Me attention dot is on the Me nav-rail tab; on compact, it is on the Home profile button. The Network settings row mirrors the same attention via `section.attention`. Refreshed by `BadgeAggregator` on messaging / reachability events.

Pane bodies live in `assets/views/*.rml` and mount into `#pane-body-{key}`. The nav rail mounts from `assets/views/nav_rail.rml`.

## Home landing

Home is a dedicated primary pane (`home.rml`), not the chat panel:

| Region | Content |
|--------|---------|
| Top left | Brand wordmark (`app.name`) + tagline |
| Top right (compact) | Profile button → account sheet |
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

## Desktop custom title bar

On desktop (`Platform::IsDesktop()`), the SDL window is created **borderless** and `#shell-titlebar` draws the caption (product name via `i18n:app.name`) plus window controls. Drag and edge-resize use `SDL_SetWindowHitTest` (`DesktopWindowChrome`). Mobile builds keep the native/system chrome and omit the title bar.

Control placement is platform-adaptive:

| Platform | Controls | Placement |
|----------|----------|-----------|
| **macOS** | Drawn traffic lights (close → minimize → zoom) | Leading (left); hit-test excludes left band |
| **Windows / Linux** | Icon strip (minimize / maximize-restore / close) | Trailing (right); hit-test excludes right band |

On macOS only, `DesktopWindowChrome::RefreshAppearance()` rounds the borderless `NSWindow` content view (~10pt) to match system windows; corners go square while maximized/fullscreen. The window is created with `SDL_WINDOW_TRANSPARENT` and the GL backbuffer clears to alpha 0 so clipped corners composite to the desktop (not opaque black). Do **not** put `border-radius` on `.shell-body` — that paints fake rounded UI over an opaque rectangular window and leaves black corners on Win/Linux.

Zoom / maximize both call `DesktopWindowChrome::ToggleMaximize()`. RML binds `titlebar_traffic_lights` (true on macOS desktop) to choose the cluster.

`content_top_dp` includes `titlebar_height_dp` (36dp) on desktop so `#shell-root` and banners sit below the bar. The title bar is a **platform** feature (still present when the desktop window is resized under 768dp), not expanded-layout-only.

## Desktop expanded status bar

On **desktop + expanded** layout (`Platform::IsDesktop() && layout_mode == Expanded`), `#shell-statusbar` is a thin (24dp) read-only bar at the bottom of the document (sibling of `#shell-root`). Spec: [network-status-chrome](../../projects/network-status-chrome/).

| Side | Content |
|------|---------|
| Left cluster | **Brief** · **Direct** · divider · **Help** · **Inbound** · **Load** pills (`circuit N` / `media N` when helping and count > 0) · sparse label. Click opens hybrid network-status popover. |
| Right | `statusbar_activity` — ephemeral busy text (Thinking…, tool labels, Preparing…) via `ShellHost::SetActivity` |

Data comes from `MessagingShellPorts::statusbar_cluster` (polled in `ShellHost::RefreshStatusbarCluster`). Adaptive: Client shows Brief+Direct; Node adds Help+Inbound; Load only when count > 0. Circuit/media aggregates from `RelayRuntimeStats` (`CircuitRelayService` / `MediaRelayService`).

**Click → hybrid popover (s2):** Left cluster is a hit target (`toggle_statusbar_popover`). Opens `#shell-statusbar-popover` above the bar (inspect Brief/Direct/reachability summary, help echo, UPnP when helping, helper load aggregates, last libp2p error) with **Test again** and **Open Network settings…**. No capability toggles. Escape / scrim dismiss.

`#shell-root` is inset with `bottom = statusbar_height_dp` while visible. Compact layout and mobile/tablet platforms omit the bar.

The top `.shell-activity-strip` (3dp progress pulse) remains for compact/mobile when `activity_visible`; it is hidden while the status bar is showing (`activity_visible && !statusbar_visible`) so busy state is not duplicated.

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
- **Batch C:** Activity strip → desktop expanded status bar; focus restore; docs
