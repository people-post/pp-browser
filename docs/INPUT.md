# Input handling architecture

pp-browser routes user input through four layers. Each layer has a single responsibility; feature logic does not belong in SDL glue or scattered `Context` forks.

## Layers

```
SDL events
  → integration/platform/RmlSDL::InputEventHandler  (translate only)
  → Rml::Context::Process*                    (focus, hover, click, keyboard)
  → SelectionController (selectable="text")   (static text selection)
  → base/ui/InputCoordinator                     (global keyboard shortcuts)
```

| Layer | Location | Responsibility |
|-------|----------|----------------|
| Platform | `src/render/integration/platform/` | SDL → `Context::Process*`; HiDPI; mouse position sync before button events |
| RmlUi core | `src/render/fork/Source/Core/Context.cpp`, `ClickRouting.cpp` | Focus/hover/click; browser-style press/release routing via `ClickRouting::ResolveClickTarget`; UAF-safe click dispatch |
| Fork selection | `SelectionController`, `ElementSelectableText` | Document-wide drag-select and Ctrl+C via element participation hooks |
| Editor widgets | `WidgetTextInput` | Focused `input`/`textarea` selection, cursor, IME, cut/paste |
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

## Authoring rules (chat UI)

- Mark static content containers with `selectable="text"`.
- Set `focus: none` on message bubbles and suggestion chips so `#draft-input` keeps focus.
- Suggestion buttons render inline inside assistant bubbles; they block text selection but remain clickable.

See [RML_PROFILE.md](RML_PROFILE.md) for allowed elements.

## ActionRouter vs InputCoordinator

- **ActionRouter** (`src/feature/ai/bindings/`) — RML `data-event-click` and MCP tool routing.
- **InputCoordinator** — keyboard shortcuts registered from C++ (demos, global quit).
