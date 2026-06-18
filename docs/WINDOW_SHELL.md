# Window Shell

The Window Shell replaces the old split-panel layout with a role-based, responsive container for all demos (Chat is the primary consumer).

## Subsystems

| Subsystem | Module | Role |
|-----------|--------|------|
| Layout | `ShellLayout` | Pure width/mode/visibility math |
| Navigation | `ShellHost`, `ViewCatalog` | Pane registry, overlays, DOM sync |
| Interruption | `ShellInterruption` | Escape/back/scrim dismiss ordering |
| Feedback | `ShellFeedback` | Toast, banner, alert/confirm dialog |

Shared state lives in `ShellState` (`ShellTypes.h`). Demos call shell APIs; they do not mount shell DOM directly.

## Pane roles

| Role | Example | Expanded | Compact |
|------|---------|----------|---------|
| Primary | Chat | Center column | Full screen |
| Secondary | Sessions | Left column | Drawer (toolbar) |
| Auxiliary | Preview | Right column when open | Sheet (toolbar) |
| Transient | Settings drill-down | Over primary | Over primary |

Layout mode switches at **768dp** width (`ShellConfig::compact_breakpoint_dp`).

## Interruption priority (high → low)

1. Dialog (alert/confirm)
2. Overlay layer (`push_layer`)
3. Transient stack
4. Auxiliary sheet (compact)
5. Secondary drawer (compact)
6. Base panes

Escape (priority 110) calls `ShellHost::HandleDismiss()` before app quit (priority 100). Toasts and banners are informational and not in the dismiss stack.

## RML / data model

Root document: `assets/samples/window_shell.rml` with `data-model="window"`.

| Callback | Action |
|----------|--------|
| `toggle_secondary()` | Open/close sessions drawer (compact) |
| `toggle_auxiliary()` | Open/close preview sheet/panel |
| `open_auxiliary()` | Open preview when available |
| `transient_back()` | Pop transient stack |
| `close_layer(id)` | Close overlay layer |
| `dismiss_banner()` | Hide banner |
| `dialog_ok()` / `dialog_cancel()` | Dialog buttons |

Pane bodies live in `assets/views/*.rml` and mount into `#pane-body-{key}`.

## Composer chrome

Primary panes may set `provides_composer = true` on `PaneSpec`. The shell mounts `assets/views/composer.rml` into a dedicated slot instead of embedding the prompt inside pane scroll content.

| Layout | Mount target | Structure |
|--------|--------------|-----------|
| Expanded | `#pane-composer-{key}` | Below `#pane-body-{key}` in the primary column |
| Compact | `#shell-composer-mount` | Inside `shell-bottom-chrome`, above the nav toolbar |

Compact bottom chrome (`shell-bottom-chrome`) stacks the prompt card and the Sessions / Preview toolbar row. Both are hidden during transient views.

The composer uses the pane's data model (`data-model="chat"` in Chat demo) for `draft` binding and `send_message()`.

## C++ usage

```cpp
ShellHost::Instance().RegisterPane({
    .key = "chat",
    .rml_path = "views/chat.rml",
    .role = PaneRole::Primary,
    .provides_composer = true,
});
ShellHost::Instance().SyncLayout();

ShellFeedback::ShowBanner(ShellHost::Instance().State(), "Offline");
ShellFeedback::ShowConfirm(ShellHost::Instance().State(), "Title", "Message", [](bool ok) { ... });
```

Call `ShellHost::Update(context)` each frame (resize, toast expiry).

## Batches

- **Batch A:** Layout, navigation, Chat migration, DOM slots for feedback
- **Batch B:** Full interruption, feedback channels, confirm/banner in Chat
- **Batch C:** Activity strip, focus restore, docs
