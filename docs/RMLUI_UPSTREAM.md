# RmlUi hard fork

pp-browser vendors RmlUi under `thirdparty/rmlui/` as a **hard fork** (committed source, no git submodule).

## Provenance

See `thirdparty/rmlui/UPSTREAM.json` for the upstream tag and commit SHA.

## Patching

Edit files under `thirdparty/rmlui/` directly in pp-browser commits.

**pp-browser fork patches (as of import):**

- `CMakeLists.txt` — wrap `add_subdirectory("Samples")` in `if(RMLUI_SAMPLES)` (Samples tree excluded from hard fork)

pp-browser-owned integration code lives outside the fork:

- `src/render/backends/` — SDL3 + OpenGL3 backend copies
- `src/app/` — application lifecycle

## Upstream sync

1. Run `./scripts/import-rmlui.sh <tag>`
2. Review diff; re-apply pp-browser-specific patches if needed
3. Diff `src/render/backends/` against `thirdparty/rmlui/Backends/`
4. Rebuild on Linux, Windows, and macOS
5. Update `UPSTREAM.json` (script does this automatically)

## License

RmlUi is MIT licensed. See `thirdparty/rmlui/LICENSE.txt`.
