# RmlUi hard fork

pp-browser vendors RmlUi under `src/render/` as a **hard fork** (committed source, no git submodule).

## Provenance

See `src/render/UPSTREAM.json` for the upstream tag and commit SHA.

## Patching

Edit files under `src/render/` directly in pp-browser commits (except `src/render/integration/`, which is pp-browser-owned SDL/GL glue).

**pp-browser fork patches (as of import):**

- `CMakeLists.txt` — wrap `add_subdirectory("Samples")` in `if(RMLUI_SAMPLES)` (Samples tree excluded from hard fork)
- `TextSelectionController` — selectable static text via `selectable="text"` attribute; integrated in `Context`

pp-browser-owned integration code:

- `src/render/integration/` — SDL3 + OpenGL3 backend copies
- `src/app/` — application lifecycle

## License

RmlUi is MIT licensed. See `src/render/LICENSE.txt`.
