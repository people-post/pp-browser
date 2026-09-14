# UI engine (pp-cpp-ui)

**Tier:** architecture

pp-browser consumes the first-party UI engine from sibling / FetchContent [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (RmlUi-derived; no git submodule). Public API is `#include <ui/…>` and `namespace ui` — there are no `RmlUi/` / `Rml::` shims.

## Layout

| Path | Role |
|------|------|
| `../pp-cpp-ui/include/ui/` | Public headers (`ui/dom`, `ui/base`, `ui/data`, `ui/widgets`, …) |
| `../pp-cpp-ui/src/` | Engine modules (`base`, `style`, `layout`, `dom`, `font`, …) |
| `../pp-cpp-ui/src/platform`, `src/render` | `Platform_SDL` + `Renderer_GL3` (`pp_ui_backend`) |
| `../pp-cpp-ui/tests/` | Unit tests (doctest) + support harness |
| `src/foundation/platform/ui/gl/` | Mobile GL lifecycle helpers |
| `src/foundation/platform/ui/renderer/` | Product overlays (text loupe, call video tiles) |
| `src/foundation/platform/ui/host/` | `BrowserHost` product `Backend::*` bootstrap |

Dependency rule:

```
foundation/platform/ui/host → pp_ui_backend (Platform_SDL + Renderer_GL3) → ui::core (public API)
```

## Provenance

Engine code started as an RmlUi 6.2 hard fork and is now owned first-party source in pp-cpp-ui.
See `../pp-cpp-ui/docs/PROVENANCE.md` and [ADR 001](https://github.com/people-post/pp-cpp-ui/blob/develop/docs/ADR_001_FIRST_PARTY_LAYOUT.md).

## Build flags / product profile

Supported knobs are `PP_BROWSER_*` plus the pp-cpp-ui pin in [`cmake/PpCppUi.cmake`](../../cmake/PpCppUi.cmake). Prefer a sibling `../pp-cpp-ui` checkout when integrating unreleased `develop`; FetchContent falls back to `PP_CPP_UI_GIT_TAG` (release tags on `main`).

| Option | Effect |
|--------|--------|
| `PP_UI_BUILD_TESTS` (pp-cpp-ui) | Builds and runs `ui_unit_tests` in the **pp-cpp-ui** repo CI — not in pp-browser |

Fixed profile policy (not options): static libs; SVG plugin on; HarfBuzz font engine on; samples / Lua / Lottie off. Headless builds skip the UI subtree entirely.

CMake targets: `ui::core`, `ui::debugger`, `pp_ui_core`, `pp_ui_backend`, `pp_ui`. Paths: `PP_LIB_UI_ROOT` / `PP_LIB_UI_INCLUDE`. Macros: `UI_*` (e.g. `UI_SDL_VERSION_MAJOR`, `UI_ASSERT`).

## Tests

UI engine unit tests run in **pp-cpp-ui** with `-DPP_UI_BUILD_TESTS=ON`. Fork-specific click-routing coverage lives under `src/dom/tests/ClickRouting_test.cpp`. pp-browser does **not** enable `UI_TESTS` (avoids EXCLUDE_FROM_ALL ctest stubs).

```bash
# From pp-cpp-ui:
cmake -S . -B build -DPP_UI_BUILD_TESTS=ON
cmake --build build --target ui_unit_tests
ctest --test-dir build --output-on-failure
```

## Data binding contract

Guaranteed after `DirtyVariable` + `DataModel::Update` (or the MountInner flush: `UpdateDocument` then `DataModelHandle::Update`). Covered by pp-cpp-ui data-binding unit tests.

| Mechanism | Contract |
|-----------|----------|
| `data-if` | Only local `display:none` counts as data-if-hidden. Local `display:flex\|block` must not block Dirty toggles. After Update, `IsVisible` matches (eager visibility via `ApplyLocalVisibilityOverrides`). |
| `data-attr-X` | Attribute `X` equals the expression string when it changes. |
| `data-class-C` | Class `C` is set iff the expression is true. |
| SVG + `data-attr-src` | `src` updates and the SVG reloads for the new path (`EnsureSourceLoaded` via layout/render). |
| `SetInnerRML` + model flush | Newly attached views apply in the same flush used by app `RmlMount::MountInner` — without waiting for a later full `Context::Update`. |

**Call-bar icons (intended pattern):** one `<svg>` with `data-attr-src` plus button `data-class-*-–on`. Dual `data-if` SVGs are not the supported icon pattern.

Do not land speculative engine data-binding patches without a failing unit test under this contract.

## Patching

Edit files under **pp-cpp-ui** `include/` / `src/` (separate repo commits). Product host/overlays stay in `src/foundation/platform/ui/`.

### Fork features (pp-browser usage)

| Feature | Location (in pp-cpp-ui) | Usage |
|---------|-------------------------|--------|
| Text selection in static content | `include/ui/dom/SelectionController.h`, `SelectionTypes.h` | RML attribute `selectable="text"`; participation API on `Element`; Ctrl+C copies selection |
| User-agent baseline styles | `src/dom/UserAgentStyleSheet.*` | Auto-merged into every document; author RCSS overrides |

**Workaround marker locations** (search `FORK_WORKAROUND` in pp-cpp-ui):

- List marker generation / first-line prepend on `li`

## Integration note (sibling develop)

Integration against unreleased pp-cpp-ui `develop` uses the sibling checkout (`../pp-cpp-ui`). After that branch is green in pp-browser CI, cut a release tag on pp-cpp-ui `main` and bump `PP_CPP_UI_GIT_TAG` here.
