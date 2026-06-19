# Input handling architecture

pp-browser routes user input through four layers. Each layer has a single responsibility; feature logic does not belong in SDL glue or scattered `Context` forks.

## Layers

```
SDL events
  → integration/RmlSDL::InputEventHandler     (translate only)
  → Rml::Context::Process*                    (focus, hover, click, keyboard)
  → SelectionController (selectable="text")   (static text selection)
  → app/InputCoordinator                      (global keyboard shortcuts)
```

| Layer | Location | Responsibility |
|-------|----------|----------------|
| Platform | `src/render/integration/` | SDL → `Context::Process*`; HiDPI; mouse position sync before button events |
| RmlUi core | `src/render/Source/Core/Context.cpp` | Standard focus/hover/click; retained fork bugfixes (UAF-safe click, geometry click for `focus:none` controls) |
| Fork selection | `SelectionController`, `ElementSelectableText` | Document-wide drag-select and Ctrl+C via element participation hooks |
| Editor widgets | `WidgetTextInput` | Focused `input`/`textarea` selection, cursor, IME, cut/paste |
| Application | `src/app/InputCoordinator.*` | Shortcuts not declared in RML (Escape quit, Enter-to-send) |

**Selection interaction** is split: read-only bubbles use `SelectionController` (drag without stealing focus from the composer); editors use `WidgetTextInput` when focused. **Selection rendering** is shared: `SelectionHighlight` resolves colors from RCSS `selection` rules and builds highlight quads; static text paints per `ElementText`, editors paint during `FormatElement`.

`src/render/Backends/` is an upstream reference copy. The running app compiles only `src/render/integration/`. Edit integration files for runtime behavior; do not dual-edit Platform SDL in `Backends/`.

## Event flow (keyboard)

1. `Backend::ProcessEvents` receives `SDL_EVENT_KEY_DOWN`.
2. **Priority phase:** `InputCoordinator::ProcessKeyDown(..., priority=true)` — Escape, Enter-send when draft is focused.
3. **RmlUi:** `Context::ProcessKeyDown` — `SelectionController` Ctrl+C, then focus `keydown`.
4. **Fallback phase:** `InputCoordinator::ProcessKeyDown(..., priority=false)` — reserved for lower-priority shortcuts.

Returning `false` from a binding or callback means the event is consumed.

## Event flow (pointer)

1. SDL mouse events → `InputEventHandler` → `Context::ProcessMouse*`.
2. `ProcessMouseButtonDown` refreshes hover, updates focus, starts selection via `SelectionController::OnPointerDown`, dispatches `mousedown`.
3. `ProcessMouseMove` calls `SelectionController::OnPointerMove` while dragging.
4. `ProcessMouseButtonUp` calls `SelectionController::OnPointerUp`, then dispatches `click` to `active` (with geometry check for non-interactive targets).

## Authoring rules (chat UI)

- Mark static content containers with `selectable="text"`.
- Set `focus: none` on message bubbles and suggestion chips so `#draft-input` keeps focus.
- Suggestion buttons render inline inside assistant bubbles; they block text selection but remain clickable.

See [RML_PROFILE.md](RML_PROFILE.md) for allowed elements.

## ActionRouter vs InputCoordinator

- **ActionRouter** (`src/bindings/`) — RML `data-event-click` and MCP tool routing.
- **InputCoordinator** — keyboard shortcuts registered from C++ (demos, global quit).
