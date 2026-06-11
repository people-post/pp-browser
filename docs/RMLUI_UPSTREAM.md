# RmlUi hard fork

pp-browser vendors RmlUi under `src/render/` as a **hard fork** (committed source, no git submodule).

## Provenance

See `src/render/UPSTREAM.json` for the upstream tag and commit SHA.

## Patching

Edit files under `src/render/` directly in pp-browser commits (except `src/render/integration/`, which is pp-browser-owned SDL/GL glue).

**pp-browser fork patches (as of import):**

- `CMakeLists.txt` — wrap `add_subdirectory("Samples")` in `if(RMLUI_SAMPLES)` (Samples tree excluded from hard fork)
- `ElementSelectableText` — selectable static text via `selectable="text"` on `<div>`; capture-phase listeners (not `Context`-integrated)
- `DataViewFor` — clone inner markup from template children when `rmlui-inner-rml` is absent (fixes empty `data-for` buttons with `{{expr}}` text)
- `UserAgentStyleSheet` / `WidgetScroll` — scrollbar cross-axis sizing so layout boxes match painted thumbs (fixes full-width invisible hit targets)
- `Context` — `PreferContentOverScrollbar` when scroll widgets overlap content at the pointer
- `Context` pointer/click — geometry-based click synthesis on mouseup (`active->IsPointWithinElement`); `FindInteractiveElement` routes clicks for `focus:none` buttons; post-layout hover refresh after data-bound DOM changes; UAF-safe `ResetActiveChain` before click dispatch
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
| Form controls | Native widget UA styles | Still styled in app theme (`base.rcss`) | Add input/button/textarea defaults to UA sheet |
| Replaced elements | `img`, etc. | Not in UA sheet | Add when needed |

**Do not reintroduce app-level layout patches** (e.g. `display: block` on `.bubble-assistant h2`) or parser hacks (bullet characters in `StructuredTextParser`) — fix gaps in the fork instead.

**Workaround marker locations** (search `FORK_WORKAROUND` in `src/render/`):

- `src/render/Source/Core/ListMarker.*` — marker string generation
- `src/render/Source/Core/Layout/InlineLevelBox.cpp` — prepends marker to first text line of `li`

**Proper fix direction for lists:** add RCSS `list-style-type` (and eventually `::marker` or an equivalent marker box) so markers participate in layout, selection, and RTL like browsers.

pp-browser-owned integration code:

- `src/render/integration/` — SDL3 + OpenGL3 backend copies (**compiled into the app**)
- `src/render/Backends/` — upstream sample backends (**reference only**; not linked). Do not dual-edit `RmlUi_Platform_SDL.cpp` here; mirror changes in `integration/` if needed.
- `src/app/` — application lifecycle and `InputCoordinator`

See [INPUT.md](INPUT.md) for the full input architecture.

## License

RmlUi is MIT licensed. See `src/render/LICENSE.txt`.
