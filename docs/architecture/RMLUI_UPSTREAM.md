# RmlUi hard fork

**Tier:** architecture

pp-browser vendors RmlUi under `src/lib/rmlui/` as a **hard fork** (committed source, no git submodule).

## Layout

| Path | Role |
|------|------|
| `src/lib/rmlui/` | Upstream-shaped RmlUi (`Include/`, `Source/`, `CMake/`, `Tests/`, minimal `Samples/`) |
| `src/lib/rmlui/Tests/` | Upstream RmlUi unit tests (doctest); fork-specific `ClickRouting.cpp` |
| `src/lib/rmlui/Samples/shell/` | Test harness utility (`rmlui_shell`); not linked by the app |
| `src/lib/rmlui/Samples/assets/` | Fonts and minimal RML/RCSS for the test harness |
| `src/lib/rmlui/reference/backends/` | Upstream sample backends (**reference only**; not linked) |
| `src/base/render/platform/` | SDL platform adapter (compiled into `pp_base_render`) |
| `src/base/render/renderer/` | OpenGL3 render interface |
| `src/base/render/host/` | `BrowserHost` bootstrap |

Dependency rule:

```
base/render/host → base/render/platform + base/render/renderer → lib/rmlui/Include (public API)
```

## Provenance

See `src/lib/rmlui/UPSTREAM.json` for the upstream tag and commit SHA. Re-import test trees with `./scripts/rmlui_tests_import.sh` when bumping the fork version.

## Tests

When `PP_BROWSER_BUILD_TESTS` is on, pp-browser builds upstream `rmlui_unit_tests` (doctest) from `src/lib/rmlui/Tests/`. Fork-specific click-routing coverage lives in `Tests/Source/UnitTests/ClickRouting.cpp`. Visual tests and benchmarks are gated off by default (`RMLUI_VISUAL_TESTS`, `RMLUI_BENCHMARKS`).

```bash
ctest --test-dir build -R rmlui_unit_tests --output-on-failure
ctest --test-dir build -R ClickRouting --output-on-failure
```

## Data binding contract

Guaranteed after `DirtyVariable` + `DataModel::Update` (or the MountInner flush: `UpdateDocument` then `DataModelHandle::Update`). Covered by `Tests/Source/UnitTests/DataBinding.cpp`.

| Mechanism | Contract |
|-----------|----------|
| `data-if` | Only local `display:none` counts as data-if-hidden. Local `display:flex\|block` must not block Dirty toggles. After Update, `IsVisible` matches (eager visibility via `ApplyLocalVisibilityOverrides`). |
| `data-attr-X` | Attribute `X` equals the expression string when it changes. |
| `data-class-C` | Class `C` is set iff the expression is true. |
| SVG + `data-attr-src` | `src` updates and the SVG reloads for the new path (`EnsureSourceLoaded` via layout/render). |
| `SetInnerRML` + model flush | Newly attached views apply in the same flush used by app `RmlMount::MountInner` — without waiting for a later full `Context::Update`. |

**Call-bar icons (intended pattern):** one `<svg>` with `data-attr-src` plus button `data-class-*-–on`. Dual `data-if` SVGs are not the supported icon pattern.

Do not land speculative fork data-binding patches without a failing unit test under this contract.

## Patching

Edit files under `src/lib/rmlui/` directly in pp-browser commits (except `src/base/render/`, which is pp-browser-owned SDL/GL glue).

**pp-browser fork patches (as of import):**

- `CMakeLists.txt` — wrap `add_subdirectory("Samples")` in `if(RMLUI_SAMPLES)`; add `RMLUI_TESTS` option for embedded pp-browser builds; import minimal `Samples/shell` when tests are on
- `CMake/DependenciesForBackends.cmake` — reuse parent `SDL::SDL` alias when pp-browser provides vendored SDL3
- `Samples/shell/` — `reference/backends` include path; `RMLUI_SAMPLES_ROOT` compile-time samples root for tests
- `Tests/Source/UnitTests/ElementDocument.cpp` — `ReloadStyleSheet` uses bundled `assets/demo.rml` instead of excluded sample demos
- `ElementSelectableText` — selectable static text via `selectable="text"` on containers; participates in document `SelectionController`
- `SelectionController` / `SelectionTypes` — participation-based static text selection (`Element::QuerySelection`, `BuildSelectionContent`, cross-container drag/copy)
- `SelectionHighlight` — shared selection background geometry and RCSS color resolution for static (`ElementText::RenderSelectionSlice`) and editor (`WidgetTextInput`) paths; lollipop **selection handle** geometry (`BuildSelectionHandleGeometry`)
- `ElementSelectableText` — `GetAbsolutePositionForFlatIndex`, handle rendering after child text (`Render` overlay pass)
- `SelectionController` — draggable selection handles (`HitTestHandle`, `BeginHandleDrag`, `UpdateHandleDrag`) for static text
- `WidgetTextInput` — composer selection handles with the same visual and drag semantics (`OnRenderOverlays` after text children); `SetValue` no-ops when the displayed string is unchanged (avoids IME/cursor reset from data-model write-back while typing); Left/Right/Backspace move by **grapheme cluster** (emoji ZWJ, skin tones, VS16, regional-indicator flags) via `StringUtilities::SeekForward/BackwardGraphemeCluster`; `SetSelectionRange` may place the caret while unfocused (no OSK activate) so in-app emoji insert can advance the composer caret without stealing focus from the emoji panel
- `StringUtilities` — grapheme-cluster seeks for mobile OS emoji keyboards (not full ICU UAX #29)
- `Element::Render` — virtual so form controls and selectable text can draw ink above descendants
- `ElementSelectableText` / `WidgetTextInput` — hidden `selection` style-probe child; theme via descendant `selection { background-color; color; }` in author RCSS
- `DataViewIf` / `DataViewVisible` — treat only `display:none` / `visibility:hidden` as data-bound hidden (not any local display/visibility); fixes Dirty toggles when layout set `display:flex|block`
- `Element::ApplyLocalVisibilityOverrides` — eager `visible`/stacking sync when Display/Visibility inline style changes during `DataModel::Update` (before `Element::UpdateProperties`); `ElementSVG::OnRender` skips when not visible
- `DataModelHandle::Update` — flush dirty variables and newly attached views (used by `RmlMount::MountInner` after `SetInnerRML`)
- `DataViewFor` — clone inner markup from template children when `rmlui-inner-rml` is absent (fixes empty `data-for` buttons with `{{expr}}` text)
- `DataView` / `DataViews` — evict stale views in `OnElementRemove` via `IsValid()` (avoids warning spam during pane remounts); `GetElement()` returns null silently like `DataController`
- `UserAgentStyleSheet` / `WidgetScroll` — scrollbar cross-axis sizing so layout boxes match painted thumbs (fixes full-width invisible hit targets)
- `Context` — `PreferContentOverScrollbar` when scroll widgets overlap content at the pointer
- `ClickRouting` / `Context` pointer/click — browser-style click synthesis: `active` stores deepest press hover; `ClickRouting::ResolveClickTarget` dispatches to the release hover when press/release share a DOM branch (tier 1), promotes to the shared interactive ancestor on layout drift (tier 2, `focus:none` chips), with focus/geometry fallbacks (tier 3); post-layout hover refresh after data-bound DOM changes; UAF-safe `ResetActiveChain` before click dispatch; `OnElementDetach` clears `active` / `last_click_element` even when the element is not yet in `active_chain` (safe mid-mousedown DOM teardown, e.g. context-menu dismiss)
- `Context` / `SelectionController` touch — iOS-aligned static text gestures: defer selection on touch down, word select on long-press/double-tap, scroll-first vertical drag; `SetTouchLongPressCallback` for app context menu; `cursor: text` UA for `input`/`textarea`; `TouchState` holds `ObserverPtr` for `touch_target` / `scroll_container` and `OnElementRemove` clears both (avoids UAF in `UpdateTouchGestures` when OSK/safe-area remounts mid-gesture)
- `Context` touch hover — after the last finger lifts (`ProcessTouchEnd` / `ProcessTouchCancel` when `touch_states` is empty), call `ProcessMouseLeave` so `:hover` does not stick at the last touch point across remount/layout shifts (e.g. compact nav pill reflow)
- `ScrollController` / `Context` touch — fling velocity from a short sample ring; softer inertia coast; rubber-band overscroll while dragging; spring settle (`Mode::Overscroll`) on release / edge hit; `Element::SetScrollTop/Left(..., clamp)` for unclamped overscroll; restore after layout clamp; axis gated when `overflow:auto` has no range; per-edge overscroll mask (`SetScrollOverscrollEdges`) so sheet dismiss can block top bounce
- `TextLoupe` / `Context` — touch-only magnifier during text selection drags (static text and `WidgetTextInput`); two-phase `SetTextLoupeRenderCallback` hook in `Context::Render()`; GL capture/draw in `src/base/render/renderer/TextLoupeRenderer.cpp`
- `WidgetDropDown` — set `:checked` on parent `<select>` while the list is open (matches RmlUi style-guide selectors)
- `UserAgentStyleSheet` — built-in baseline RCSS merged into every document (block layout for `p`, headings, lists, tables)
- `ListMarker` — **workaround**: layout-time bullet/number injection (see limitations below)
- `ResolveValueOr` / `FlexFormattingContext` / `BuildBoxWidth` / `GetShrinkToFitWidth` — percentage and auto width no longer collapse to 0px when the containing block is indefinite or zero-sized
- `FontEngineHarfBuzz/` — HarfBuzz text shaping engine ported from upstream `Samples/basic/harfbuzz`; enabled via `RMLUI_FONT_ENGINE_HARFBUZZ` (on by default in pp-browser builds); script is detected from string content (not forced from UI `lang`), and CJK UI `lang` is not applied to non-CJK runs, so Latin inputs stay stable when the document is `lang=zh-Hans`
- `position: sticky` — `Style::Position::Sticky` (ComputedValues bitfield widened to 3 bits); stays in normal flow (block/flex/table like relative); `Element::ComputeStickyOffset` clamps against the nearest overflow scrollport and parent padding box; recomputed when absolute offsets are dirtied on scroll; sticky is a positioned containing block for absolute descendants (CSS); tests in `Tests/Source/UnitTests/Layout.cpp` (`Layout.Position.Sticky`)

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

**Workaround marker locations** (search `FORK_WORKAROUND` in `src/lib/rmlui/` and `src/base/render/`):

- `src/lib/rmlui/Source/Core/ListMarker.*` — marker string generation
- `src/lib/rmlui/Source/Core/Layout/InlineLevelBox.cpp` — prepends marker to first text line of `li`

**Proper fix direction for lists:** add RCSS `list-style-type` (and eventually `::marker` or an equivalent marker box) so markers participate in layout, selection, and RTL like browsers.

pp-browser-owned integration code:

- `src/base/render/` — SDL3 + OpenGL3 backend (**compiled into the app** via `pp_base_render`)
- `src/lib/rmlui/reference/backends/` — upstream sample backends (**reference only**; not linked). Do not dual-edit `RmlUi_Platform_SDL.cpp` here; mirror changes in `integration/platform/` if needed.
- `src/app/` — application lifecycle and `InputCoordinator`

See [INPUT.md](../ui/INPUT.md) for the full input architecture.

## License

RmlUi is MIT licensed. See `src/lib/rmlui/LICENSE.txt`.
