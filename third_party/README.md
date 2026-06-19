# Third-party source dependencies

Vendored upstream libraries built via `add_subdirectory` from [`cmake/dependencies.cmake`](../cmake/dependencies.cmake).

RmlUi remains a hard fork under [`src/render/`](../src/render/), not in this directory.

libp2p itself is a hard fork under [`src/libp2p/`](../src/libp2p/), not here. This directory holds **libp2p's external dependencies**.

## Libraries

| Directory | Upstream | Tag | License |
|-----------|----------|-----|---------|
| `freetype/` | [freetype/freetype](https://github.com/freetype/freetype) | `VER-2-13-3` | FTL / GPLv2 |
| `nlohmann_json/` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | MIT |
| `curl/` | [curl/curl](https://github.com/curl/curl) | `curl-8_11_1` | curl license |
| `sdl3/` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `release-3.2.8` | Zlib |
| `sdl3_image/` | [libsdl-org/SDL_image](https://github.com/libsdl-org/SDL_image) | `release-3.2.4` | Zlib |

### libp2p dependencies (when enabled)

Imported by [`scripts/libp2p_vendor_import.sh`](../scripts/libp2p_vendor_import.sh). See `libp2p_dependencies` in [`UPSTREAM.json`](UPSTREAM.json).

| Directory | Upstream | Notes |
|-----------|----------|-------|
| `boringssl/` | [qdrvm/boringssl](https://github.com/qdrvm/boringssl) `qdrvm1` | TLS for libp2p + curl |
| `boost/` | Boost 1.87.0 | Wrapper `CMakeLists.txt` added |
| `protobuf/` | cpp-pm/protobuf 3.19.4-p0 | Hunter stripped |
| `lsquic/` | [qdrvm/lsquic](https://github.com/qdrvm/lsquic) 4.0.9-qdrvm-1 | Patched at import |
| `libsecp256k1/` | qdrvm/libsecp256k1 0.5.1 | |
| `c-ares/` | hunter-packages/c-ares 1.14.0-p0 | |
| `fmt/` | fmtlib/fmt 10.1.1 | |
| `yaml-cpp/` | hunter-packages/yaml-cpp 0.6.2 | |
| `soralog/` | qdrvm/soralog 0.2.5 | |
| `qtils/` | qdrvm/qtils 0.1.1 | |
| `tsl_hat_trie/` | masterjedy/hat-trie | |
| `boost_di/` | qdrvm/boost-di | |
| `zlib/` | qdrvm/zlib 1.3.0-p1 | For lsquic / protobuf |

| Directory | Upstream | Tag | License |
|-----------|----------|-----|---------|
| `freetype/` | [freetype/freetype](https://github.com/freetype/freetype) | `VER-2-13-3` | FTL / GPLv2 |
| `nlohmann_json/` | [nlohmann/json](https://github.com/nlohmann/json) | `v3.11.3` | MIT |
| `curl/` | [curl/curl](https://github.com/curl/curl) | `curl-8_11_1` | curl license |
| `sdl3/` | [libsdl-org/SDL](https://github.com/libsdl-org/SDL) | `release-3.2.8` | Zlib |
| `sdl3_image/` | [libsdl-org/SDL_image](https://github.com/libsdl-org/SDL_image) | `release-3.2.4` | Zlib |

`third_party/sdl3_image/external/` contains SDL's pinned codec forks (dav1d, aom, libavif, libpng, etc.) imported from `.gitmodules` by [`scripts/vendor_import.sh`](../scripts/vendor_import.sh). These are committed as **regular source files** (`.git` metadata stripped), not as nested git submodules — a plain `git clone` of pp-browser includes them.

Exact commit SHAs are recorded in [`UPSTREAM.json`](UPSTREAM.json).

## System dependencies (not vendored)

- **Linux:** X11 and OpenGL development headers (when libp2p disabled: `libssl-dev` for curl TLS)
- **Windows:** Schannel (via curl)
- **macOS:** Secure Transport (via curl)

When libp2p is enabled, BoringSSL in `third_party/boringssl/` provides TLS for curl and libp2p.

## Updating a vendored library

1. Edit the tag in [`scripts/vendor_import.sh`](../scripts/vendor_import.sh).
2. Run `./scripts/vendor_import.sh` from the repo root.
3. Rebuild and run tests.
4. Commit `third_party/` and `UPSTREAM.json`.

Do not edit vendored trees for pp-browser features; patch upstream or wrap in app code instead.
