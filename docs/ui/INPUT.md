# Input handling architecture

**Tier:** product / UI

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
| Platform | `src/base/render/` host + pp-cpp-ui `backend/` | SDL → `Context::Process*`; HiDPI; mouse position sync before button events |
| RmlUi core | `pp-cpp-ui rmlui/Source/Core/Context.cpp`, `ClickRouting.cpp` | Focus/hover/click; iOS-aligned touch deferral for static text; long-press callback |
| Fork selection | `SelectionController`, `ElementSelectableText` | Document-wide selection; desktop drag-select; touch word select via long-press / double-tap |
| Editor widgets | `WidgetTextInput` | Focused `input`/`textarea` selection, cursor, IME, cut/paste |
| Context menu | `src/base/ui/ContextMenuHost.*` | Extensible Copy / Select All / Paste panel; desktop right-click; mobile long-press |
| Base UI | `src/base/ui/InputCoordinator.*` | Shortcuts not declared in RML (Escape quit, Enter-to-send) |

**Selection interaction** is split: read-only bubbles use `SelectionController` (drag without stealing focus from the composer); editors use `WidgetTextInput` when focused. **Selection rendering** is shared: `SelectionHighlight` resolves colors from RCSS `selection` rules and builds highlight quads; static text paints per `ElementText`, editors paint during `FormatElement`.

`pp-cpp-ui rmlui/reference/backends/` backs RmlUi unit-test shell. The running app uses pp-cpp-ui `backend/` plus product `BrowserHost` in `src/base/render/host/`. Edit product host for app runtime behavior; edit pp-cpp-ui `backend/` for shared Platform_SDL / Renderer_GL3.

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

**Scroll feel (overflow containers):**

- Finger-down tracking is 1:1, with rubber-band resistance past the top/bottom (or left/right) edges.
- Axes with `overflow: auto` and no overflow range do not move (no sideways wiggle on full-width lists).
- On lift, fling velocity is estimated from recent touch samples (~100ms window) and applied as inertial coast (hard-clamped until edge contact, then a gentle spring settle).
- Fling/settle lock to the dominant axis and never write non-scrollable axes (fixes diagonal X coast after lift).
- Overscroll settle uses a stretch-scaled spring: farther past the edge → faster snap-back.
- Releasing while overscrolled (or flinging into an edge) spring-settles back into range with a light bounce.
- Touching again interrupts coast / settle immediately.
- Account sheet pull-to-dismiss blocks top-edge rubber-band and pins list scroll at top so dismiss owns the gesture.

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

**Compact chat overlay / drill-down:**

- Swipe-back starts only from the left edge (~20dp) or overlay chrome, not from bubble content.
- Settings detail (Me sheet) uses the same edge swipe-back; vertical dismiss-from-anywhere on the sheet remains available — axis lock picks the winner after the drag deadzone.

## Selection handles

When a non-empty text range is selected, iOS-style **lollipop handles** appear at the visual start and end of the range (static chat bubbles and composer textarea).

- **Static bubbles:** `SelectionController` renders handles via `ElementSelectableText` after child text (overlay pass).
- **Composer:** `WidgetTextInput` renders handles in `OnRenderOverlays` after the internal text elements.
- **Drag start handle** moves only the range start; **drag end handle** moves only the range end.
- **Touch:** handle hits are tested before scroll slop and before content selection; handle drag sets `selection_armed` and blocks scroll.
- **Desktop:** same handle hit-test runs in `ProcessMouseButtonDown` before `OnPointerDown`.

Handles are hidden during active content drag-select; they appear after release (or immediately after long-press / double-tap word select once dragging ends).

Static handles render in `Context::Render` after the document tree, with clip masks disabled so rounded bubble corners do not stencil them away.

### Debug markers (optional)

Pass `-DRMLUI_DEBUG_SELECTION_HANDLES=ON` at configure time to draw **magenta squares with yellow crossbars** at the computed start/end anchor positions (useful if handle rendering regresses). Default is off.

## Simulated touch (desktop dev)

Build with `-DRMLUI_BACKEND_SIMULATE_TOUCH=ON` to route mouse input through the touch path (`SDL_HINT_MOUSE_TOUCH_EVENTS`) and draw a semi-transparent red contact dot instead of the OS cursor.

```bash
cmake -B build -S . -DRMLUI_BACKEND_SIMULATE_TOUCH=ON
cmake --build build -j
./build/src/app/pp-browser
```

Implementation: [`TouchSimOverlay`](src/base/render/host/TouchSimOverlay.cpp) in `pp_base_render` (compiled only when the CMake option is set). Each frame it polls `SDL_GetMouseState`, maps window coordinates to pixel space the same way as synthetic finger events (`x / window_w * pixel_w`), and draws the dot at the current pointer position while the window has mouse focus — not only during press. The overlay sets its own GL viewport from live `SDL_GetWindowSizeInPixels` so it stays aligned after window resize. Real mobile builds are unchanged.

## Context menu

- Desktop: right-click → `SdlAppEvents` → `ContextMenuHost::OnContextPointer`.
- Mobile: `Context::SetTouchLongPressCallback` → `ContextMenuHost::OnLongPress`.
- Dismiss: scrim tap, action chosen, Escape / system back (via `ShellHost::HandleDismiss`).
- Menu actions run on **mousedown** (capture phase) so child `#text` nodes do not block item hits.
- Copy uses text **snapshotted when the menu opens**, because the subsequent menu-item mousedown would otherwise clear selection before the action runs.
- After Copy / Select All / Paste, `SelectionController::FinalizeSelection()` and `OnPointerUp()` clear any lingering static-text drag state.
- Editor Select All refocuses the control and calls `SetSelectionRange(0, INT_MAX)` directly (bypasses focus guard in `Select()`). Unfocused `SetSelectionRange` updates the caret index without activating the OSK.

Register extra actions with `ContextMenuHost::RegisterProvider`.

**Chat reactions:** long-press / right-click a message row (`message-id` attribute) adds **React…**, which opens a preset emoji strip (plus **More…**). **More…** and the composer ☺ button open the in-app emoji picker (`emoji_picker`: category rail + sticky section labels + scroll-spy). On mobile/compact **Insert** uses shell **bottom chrome** (IME-replacement): latched IME height, OSK dismissed, no dimming scrim, remount-only into `#shell-emoji-keyboard-mount`. Dismiss with Back/Escape or tap ☺ again. Expanded desktop **Insert** and all **React** picks use a `PushLayer` overlay (scrim OK). On mobile/compact (**bottom chrome**), **Insert** stays open for multi-tap and advances the caret without focusing the composer (OSK stays dismissed). Expanded overlay **Insert** closes on pick; `PaneSpec.return_focus_id` restores `#draft-input`, and the caret is placed after the glyph. **React** picks are single-shot and close on pick. Recently used glyphs persist in profile prefs. OS paste / OSK remains available for rare glyphs not in the curated catalog. Tapping a reaction chip calls `toggle_reaction(message_id, emoji)`. Composer Left/Right/Backspace move by grapheme cluster so OSK multi-codepoint emoji edit correctly.

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
- **InputCoordinator** — keyboard shortcuts registered from C++ (global quit, etc.).
