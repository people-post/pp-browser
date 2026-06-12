# Third-party source dependencies

Vendored upstream libraries built via `add_subdirectory` from [`cmake/dependencies.cmake`](../cmake/dependencies.cmake).

RmlUi remains a hard fork under [`src/render/`](../src/render/), not in this directory.

## Libraries

| Directory | Upstream | Tag | License |
|-----------|----------|-----|---------|
| `freetype/` | [freetype/freetype](https://github.com/freetype/freetype) | `VER-2-13-3` | FTL / GPLv2 |
| `nlohmann_json/` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | MIT |
| `curl/` | [curl/curl](https://github.com/curl/curl) | `curl-8_11_1` | curl license |
| `sdl3/` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `release-3.2.8` | Zlib |
| `sdl3_image/` | [libsdl-org/SDL_image](https://github.com/libsdl-org/SDL_image) | `release-3.2.4` | Zlib |

Exact commit SHAs are recorded in [`UPSTREAM.json`](UPSTREAM.json).

## System dependencies (not vendored)

- **Linux:** OpenSSL (`libssl-dev`), X11 and OpenGL development headers
- **Windows:** Schannel (via curl)
- **macOS:** Secure Transport (via curl)

## Updating a vendored library

1. Edit the tag in [`scripts/vendor_import.sh`](../scripts/vendor_import.sh).
2. Run `./scripts/vendor_import.sh` from the repo root.
3. Rebuild and run tests.
4. Commit `third_party/` and `UPSTREAM.json`.

Do not edit vendored trees for pp-browser features; patch upstream or wrap in app code instead.
