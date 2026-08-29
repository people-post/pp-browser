# Third-party source dependencies

Vendored upstream libraries built via `add_subdirectory` from [`cmake/dependencies.cmake`](../cmake/dependencies.cmake).

RmlUi + FreeType / HarfBuzz / LunaSVG + **SDL3 / SDL3_image** live in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui), not in this directory.

libp2p itself is a hard fork under [`src/lib/libp2p/`](../src/lib/libp2p/), not here. This directory holds **libp2p's external dependencies** plus a few app libs (curl, sqlite, opus).

## Libraries

| Directory | Upstream | Tag | License |
|-----------|----------|-----|---------|
| `curl/` | [curl/curl](https://github.com/curl/curl) | `curl-8_11_1` | curl license |
| `sqlite/` | [SQLite amalgamation](https://www.sqlite.org/download.html) | `3.53.3` (`3530300`) | Public domain |
| `opus/` | [xiph/opus](https://github.com/xiph/opus) | `v1.5.2` | BSD |

JSON (`Value` / `Object`) comes from [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common). libsodium, mlkem-native, and mldsa-native live in [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto). FreeType / HarfBuzz / LunaSVG / SDL3 / SDL3_image live in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui).

### libp2p dependencies (when enabled)

Imported by [`scripts/libp2p_vendor_import.sh`](../scripts/libp2p_vendor_import.sh). See `libp2p_dependencies` in [`UPSTREAM.json`](UPSTREAM.json).

| Directory | Upstream | Notes |
|-----------|----------|-------|
| `boringssl/` | [qdrvm/boringssl](https://github.com/qdrvm/boringssl) `qdrvm1` | TLS for libp2p + curl |
| `asio/` | [chriskohlhoff/asio](https://github.com/chriskohlhoff/asio) 1.34.0 | Standalone Asio (`ASIO_STANDALONE`) |
| `outcome/` | ned14/outcome v2.2.15 | Standalone Outcome (via `qtils/outcome.hpp`) |
| `lsquic/` | [qdrvm/lsquic](https://github.com/qdrvm/lsquic) 4.0.9-qdrvm-1 | Patched at import |
| `c-ares/` | hunter-packages/c-ares 1.14.0-p0 | |
| `fmt/` | fmtlib/fmt 10.1.1 | |
| `yaml-cpp/` | hunter-packages/yaml-cpp 0.6.2 | |
| `soralog/` | qdrvm/soralog 0.2.5 | |
| `qtils/` | qdrvm/qtils 0.1.1 | |
| `tsl_hat_trie/` | masterjedy/hat-trie | |
| `zlib/` | qdrvm/zlib 1.3.0-p1 | For lsquic |

libp2p wire codecs are handwritten (`src/lib/libp2p/src/wire/`) — no vendored protobuf. Call media is libp2p-only ([V026](../projects/p2p-av-calls/DECISIONS.md#v026--libp2p-only-call-media-http--libp2p-networking)); libdatachannel is not vendored.

Exact commit SHAs are recorded in [`UPSTREAM.json`](UPSTREAM.json).

## System dependencies (not vendored)

- **Linux GUI:** X11 and OpenGL development headers (see [docs/ops/BUILD.md](../docs/ops/BUILD.md)) — required by pp-cpp-ui’s SDL3
- **Linux voice (a2+):** `libpulse-dev` + `libasound2-dev` — both required so SDL3 builds PulseAudio + ALSA drivers (not dummy-only). PipeWire desktops still need `libpulse-dev`.
- **Windows / macOS / mobile:** no Pulse/ALSA packages — WASAPI / CoreAudio / AAudio. Mobile still needs manifest/plist mic (and later camera) permissions — [PLATFORMS § A/V](../docs/architecture/PLATFORMS.md#av-media-sdl--calls).
- **Windows TLS:** Schannel (via curl)
- **macOS TLS:** Secure Transport (via curl)

When libp2p is enabled, BoringSSL in `third_party/boringssl/` provides TLS for curl and libp2p.

## Updating a vendored library

1. Edit the tag in [`scripts/vendor_import.sh`](../scripts/vendor_import.sh).
2. Run `./scripts/vendor_import.sh` from the repo root.
3. Rebuild and run tests.
4. Commit `third_party/` and `UPSTREAM.json`.

Do not edit vendored trees for pp-browser features; patch upstream or wrap in app code instead.

For SDL / RmlUi / fonts / SVG, bump the pin in [`cmake/PpCppUi.cmake`](../cmake/PpCppUi.cmake) (`PP_CPP_UI_GIT_TAG`) and update the sibling [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) repo.
