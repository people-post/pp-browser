# Building pp-browser

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 13+, Clang 16+, or MSVC 2022)
- OpenGL 3.3 drivers
- Linux: `libx11-dev`, `libxext-dev`, `libxcursor-dev`, `libxinerama-dev`, `libxi-dev`, `libxrandr-dev`, `libxfixes-dev`, `libgl-dev`
- Perl (for lsquic code generation, when libp2p build is enabled)

When libp2p is enabled (default), curl uses vendored **BoringSSL** instead of system `libssl-dev`.

## Dependencies

**Vendored source** under [`third_party/`](../third_party/): FreeType, nlohmann-json, curl, SDL3, SDL3_image, and (for libp2p) BoringSSL, Boost, Protobuf, lsquic, and related packages.

**System packages:** X11 and OpenGL development headers on Linux for the GUI.

RmlUi is **hard-forked** under `src/render/`. libp2p is **hard-forked** under `src/libp2p/` (not in `third_party/`).

If base `third_party/` trees are missing, run `./scripts/vendor_import.sh` from the repo root.

If libp2p dependency trees are missing, run `./scripts/libp2p_vendor_import.sh`.

Disable the libp2p subtree with `-DPP_BROWSER_ENABLE_LIBP2P_BUILD=OFF` (restores system OpenSSL for curl on Linux).

Codec sources under `third_party/sdl3_image/external/` are committed as **regular files** (not git submodules). If configure reports missing externals after clone, re-run `./scripts/vendor_import.sh` and ensure those directories contain source files, not empty gitlink placeholders.

## Configure and build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Run

From the repository root (assets path is compile-time `PP_BROWSER_ASSETS_DIR`):

```bash
./build/pp-browser
./build/pp-browser --demo search
```

### Local Ollama

1. Start Ollama and pull a model, e.g. `ollama pull llama3.2`
2. Copy `config.json.example` to `config.json` and set `llm.model` to your model name
3. Run `./build/pp-browser`

Ollama exposes an OpenAI-compatible API at `http://localhost:11434/v1`; no API key is required. Without `config.json`, the app defaults to Ollama on localhost with model `llama3.2` (override via `PP_BROWSER_LLM_MODEL`).

For OpenAI or other providers, set `base_url`, `model`, and `api_key_env` in `config.json`.

**If no window appears** (or exit code 1), reconfigure from a clean build directory:

```bash
rm -rf build
cmake -B build -S .
cmake --build build -j
```

Do **not** pass `-DPP_BROWSER_HEADLESS=ON` unless you only need compile-only CI builds.

Requires `DISPLAY` (or Wayland session) and X11 dev packages on Linux.

## Tests

```bash
ctest --test-dir build --output-on-failure
```
