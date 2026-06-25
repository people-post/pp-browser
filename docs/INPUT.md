# Input handling architecture

pp-browser routes user input through four layers. Each layer has a single responsibility; feature logic does not belong in SDL glue or scattered `Context` forks.

## Layers

```
SDL events
  → integration/platform/RmlSDL::InputEventHandler  (translate only)
  → Rml::Context::Process*                    (focus, hover, click, keyboard, touch gestures)
  → SelectionController (selectable="text")   (static text selection)
  → base/ui/ContextMenuHost                   (right-click / long-press edit menu)
  → base/ui/InputCoordinator                  (global keyboard shortcuts)
```

| Layer | Location | Responsibility |
|-------|----------|----------------|
| Platform | `src/render/integration/platform/` | SDL → `Context::Process*`; HiDPI; mouse position sync before button events |
| RmlUi core | `src/render/fork/Source/Core/Context.cpp`, `ClickRouting.cpp` | Focus/hover/click; iOS-aligned touch deferral for static text; long-press callback |
| Fork selection | `SelectionController`, `ElementSelectableText` | Document-wide selection; desktop drag-select; touch word select via long-press / double-tap |
| Editor widgets | `WidgetTextInput` | Focused `input`/`textarea` selection, cursor, IME, cut/paste |
| Context menu | `src/base/ui/ContextMenuHost.*` | Extensible Copy / Select All / Paste panel; desktop right-click; mobile long-press |
| Base UI | `src/base/ui/InputCoordinator.*` | Shortcuts not declared in RML (Escape quit, Enter-to-send) |

**Selection interaction** is split: read-only bubbles use `SelectionController` (drag without stealing focus from the composer); editors use `WidgetTextInput` when focused. **Selection rendering** is shared: `SelectionHighlight` resolves colors from RCSS `selection` rules and builds highlight quads; static text paints per `ElementText`, editors paint during `FormatElement`.

`src/render/fork/reference/backends/` is an upstream reference copy. The running app compiles only `src/render/integration/`. Edit integration files for runtime behavior; do not dual-edit Platform SDL in `reference/backends/`.

## Event flow (keyboard)

1. `Backend::ProcessEvents` receives `SDL_EVENT_KEY_DOWN`.
2. **Priority phase:** `InputCoordinator::ProcessKeyDown(..., priority=true)` — Escape, Enter-send when draft is focused.
3. **RmlUi:** `Context::ProcessKeyDown` — `SelectionController` Ctrl+C, then focus `keydown`.
4. **Fallback phase:** `InputCoordinator::ProcessKeyDown(..., priority=false)` — reserved for lower-priority shortcuts.

Returning `false` from a binding or callback means the event is consumed.

## Event flow (pointer)

1. SDL mouse events → `InputEventHandler` → `Context::ProcessMouse*`.
2. `ProcessMouseButtonDown` refreshes hover, stores the deepest hit as `active` (press target), updates focus, starts selection via `SelectionController::OnPointerDown`, dispatches `mousedown`.
3. `ProcessMouseMove` calls `SelectionController::OnPointerMove` while dragging.
4. `ProcessMouseButtonUp` calls `SelectionController::OnPointerUp`, then resolves and dispatches `click` via `ClickRouting::ResolveClickTarget(active, hover, …)`:
   - **Tier 1:** press and release on the same element or ancestor/descendant chain → click goes to the release hover (e.g. `<option>` inside `<select>`).
   - **Tier 2:** same interactive ancestor (`button`, `data-event-click`, form control) and pointer still inside → click goes to that control (layout drift, sibling children under a chip).
   - **Tier 3:** non-interactive press with focus or geometry fallback.

## Event flow (touch)

On touch devices (`SDL_HINT_TOUCH_MOUSE_EVENTS=0`), finger events map to `Context::ProcessTouch*`.

**Read-only chat bubbles (iOS Messages / Safari):**

- Finger down does **not** start selection.
- Vertical drag scrolls; selection is not armed.
- Long press (~500ms) selects the word under the finger and opens `ContextMenuHost`.
- Double-tap selects the word without opening the menu.
- Drag after long-press / double-tap extends the selection.
- Quick tap outside an existing selection clears it.

**Composer textarea (iOS UITextView):**

- Tap places the caret via `WidgetTextInput` (touch still synthesizes mouse down).
- Long press opens the edit menu (Copy / Paste / Select All).

**Compact chat overlay:**

- Swipe-back starts only from the left edge (~20dp) or overlay chrome, not from bubble content.

## Context menu

- Desktop: right-click → `SdlAppEvents` → `ContextMenuHost::OnContextPointer`.
- Mobile: `Context::SetTouchLongPressCallback` → `ContextMenuHost::OnLongPress`.
- Dismiss: scrim tap, action chosen, Escape / system back (via `ShellHost::HandleDismiss`).
- Menu actions run on **mousedown** (capture phase) so child `#text` nodes do not block item hits.
- Copy uses text **snapshotted when the menu opens**, because the subsequent menu-item mousedown would otherwise clear selection before the action runs.
- After Copy / Select All / Paste, `SelectionController::FinalizeSelection()` and `OnPointerUp()` clear any lingering static-text drag state.
- Editor Select All refocuses the control and calls `SetSelectionRange(0, INT_MAX)` directly (bypasses focus guard in `Select()`).

Register extra actions with `ContextMenuHost::RegisterProvider`.

## Textarea selection

- **Triple-click** selects the paragraph around the caret using blank-line (`\n\n`) boundaries.
- **Drag-select** across lines uses `EventId::Drag`; static-text drag is cleared on editor mousedown so textarea drag is not blocked.
- While drag-selecting, the pointer within ~12dp of the top/bottom edge auto-scrolls the textarea and extends the selection into off-screen lines.

## Authoring rules (chat UI)

- Mark static content containers with `selectable="text"`.
- Set `focus: none` on message bubbles and suggestion chips so `#draft-input` keeps focus.
- Suggestion buttons render inline inside assistant bubbles; they block text selection but remain clickable.

See [RML_PROFILE.md](RML_PROFILE.md) for allowed elements.

## ActionRouter vs InputCoordinator

- **ActionRouter** (`src/feature/ai/bindings/`) — RML `data-event-click` and MCP tool routing.
- **InputCoordinator** — keyboard shortcuts registered from C++ (demos, global quit).
