# RmlUi hard fork

pp-browser vendors RmlUi under `src/render/fork/` as a **hard fork** (committed source, no git submodule).

## Layout

| Path | Role |
|------|------|
| `src/render/fork/` | Upstream-shaped RmlUi (`Include/`, `Source/`, `CMake/`) |
| `src/render/fork/reference/backends/` | Upstream sample backends (**reference only**; not linked) |
| `src/render/integration/platform/` | SDL platform adapter (compiled into `pp_rmlui_backend`) |
| `src/render/integration/renderer/` | OpenGL3 render interface |
| `src/render/integration/host/` | `BrowserHost` bootstrap |

Dependency rule inside the render subtree:

```
integration/host → integration/platform + integration/renderer → fork/Include (public API)
```

## Provenance

See `src/render/fork/UPSTREAM.json` for the upstream tag and commit SHA.

## Patching

Edit files under `src/render/fork/` directly in pp-browser commits (except `src/render/integration/`, which is pp-browser-owned SDL/GL glue).

**pp-browser fork patches (as of import):**

- `CMakeLists.txt` — wrap `add_subdirectory("Samples")` in `if(RMLUI_SAMPLES)` (Samples tree excluded from hard fork)
- `ElementSelectableText` — selectable static text via `selectable="text"` on containers; participates in document `SelectionController`
- `SelectionController` / `SelectionTypes` — participation-based static text selection (`Element::QuerySelection`, `BuildSelectionContent`, cross-container drag/copy)
- `SelectionHighlight` — shared selection background geometry and RCSS color resolution for static (`ElementText::RenderSelectionSlice`) and editor (`WidgetTextInput`) paths; lollipop **selection handle** geometry (`BuildSelectionHandleGeometry`)
- `ElementSelectableText` — `GetAbsolutePositionForFlatIndex`, handle rendering after child text (`Render` overlay pass)
- `SelectionController` — draggable selection handles (`HitTestHandle`, `BeginHandleDrag`, `UpdateHandleDrag`) for static text
- `WidgetTextInput` — composer selection handles with the same visual and drag semantics (`OnRenderOverlays` after text children)
- `Element::Render` — virtual so form controls and selectable text can draw ink above descendants
- `ElementSelectableText` / `WidgetTextInput` — hidden `selection` style-probe child; theme via descendant `selection { background-color; color; }` in author RCSS
- `DataViewFor` — clone inner markup from template children when `rmlui-inner-rml` is absent (fixes empty `data-for` buttons with `{{expr}}` text)
- `DataView` / `DataViews` — evict stale views in `OnElementRemove` via `IsValid()` (avoids warning spam during pane remounts); `GetElement()` returns null silently like `DataController`
- `UserAgentStyleSheet` / `WidgetScroll` — scrollbar cross-axis sizing so layout boxes match painted thumbs (fixes full-width invisible hit targets)
- `Context` — `PreferContentOverScrollbar` when scroll widgets overlap content at the pointer
- `ClickRouting` / `Context` pointer/click — browser-style click synthesis: `active` stores deepest press hover; `ClickRouting::ResolveClickTarget` dispatches to the release hover when press/release share a DOM branch (tier 1), promotes to the shared interactive ancestor on layout drift (tier 2, `focus:none` chips), with focus/geometry fallbacks (tier 3); post-layout hover refresh after data-bound DOM changes; UAF-safe `ResetActiveChain` before click dispatch
- `Context` / `SelectionController` touch — iOS-aligned static text gestures: defer selection on touch down, word select on long-press/double-tap, scroll-first vertical drag; `SetTouchLongPressCallback` for app context menu; `cursor: text` UA for `input`/`textarea`
- `WidgetDropDown` — set `:checked` on parent `<select>` while the list is open (matches RmlUi style-guide selectors)
- `UserAgentStyleSheet` — built-in baseline RCSS merged into every document (block layout for `p`, headings, lists, tables)
- `ListMarker` — **workaround**: layout-time bullet/number injection (see limitations below)
- `ResolveValueOr` / `FlexFormattingContext` / `BuildBoxWidth` / `GetShrinkToFitWidth` — percentage and auto width no longer collapse to 0px when the containing block is indefinite or zero-sized

### User-agent baseline: browser comparison and known gaps

Upstream RmlUi defaults every element to `display: inline` (`StyleSheetSpecification.cpp`) and expects apps to link `rml.rcss` manually. Our fork adds a built-in user-agent sheet merged in `ElementDocument::ProcessHeader` before author RCSS (e.g. `assets/themes/base.rcss`).

| Area | Browsers | Our fork (current) | Improve later |
|------|----------|-------------------|---------------|
| Baseline layout | Large per-engine UA stylesheet | Minimal embedded RCSS in `UserAgentStyleSheet.cpp` | Expand coverage; consider upstreaming |
| Cascade | UA → user → author | UA → author only (no user tier) | Optional user-style tier if needed |
| Opt-out | Difficult | None (always merged) | `Rml::SetUserAgentStylesEnabled(false)` or similar |
| List markers | `list-style`, `::marker`, CSS counters | **Workaround** in `ListMarker.cpp` + `InlineLevelBox.cpp` | Implement `list-style` / marker box in layout |
| List marker scope | Any `li` content structure | Direct text child of `li` only (`<li>text</li>`) | Marker on first line of nested blocks (`<li><p>…`) |
| Ordered lists | `list-style-type: decimal` etc. | Hard-coded `1. ` prefix | CSS counters / `start` attribute |
| Form controls | Native widget UA styles | `cursor: text` on `textarea`/`input` in UA sheet; tag fallback in `GetEffectiveCursor` | Add more input/button/textarea defaults to UA sheet |
| Replaced elements | `img`, etc. | Not in UA sheet | Add when needed |

**Do not reintroduce app-level layout patches** (e.g. `display: block` on `.bubble-assistant h2`) or parser hacks (bullet characters in `StructuredTextParser`) — fix gaps in the fork instead.

**Workaround marker locations** (search `FORK_WORKAROUND` in `src/render/`):

- `src/render/fork/Source/Core/ListMarker.*` — marker string generation
- `src/render/fork/Source/Core/Layout/InlineLevelBox.cpp` — prepends marker to first text line of `li`

**Proper fix direction for lists:** add RCSS `list-style-type` (and eventually `::marker` or an equivalent marker box) so markers participate in layout, selection, and RTL like browsers.

pp-browser-owned integration code:

- `src/render/integration/` — SDL3 + OpenGL3 backend (**compiled into the app** via `pp_rmlui_backend`)
- `src/render/fork/reference/backends/` — upstream sample backends (**reference only**; not linked). Do not dual-edit `RmlUi_Platform_SDL.cpp` here; mirror changes in `integration/platform/` if needed.
- `src/app/` — application lifecycle and `InputCoordinator`

See [INPUT.md](INPUT.md) for the full input architecture.

## License

RmlUi is MIT licensed. See `src/render/fork/LICENSE.txt`.
