# Building pp-browser

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 13+, Clang 16+, or MSVC 2022)
- OpenSSL development headers (Linux only; `libssl-dev` on Debian/Ubuntu)
- OpenGL 3.3 drivers
- Linux: `libx11-dev`, `libxext-dev`, `libxcursor-dev`, `libxinerama-dev`, `libxi-dev`, `libxrandr-dev`, `libxfixes-dev`, `libgl-dev`

## Dependencies

**FetchContent (first configure):** FreeType, nlohmann-json, curl, SDL3, and SDL3_image (SDL built with `SDL_DBUS=OFF` to avoid extra system autotools)

RmlUi is **hard-forked** under `src/render/` (not from vcpkg).

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

**If no window appears** (or exit code 1), SDL was likely built without X11. Rebuild SDL with video support:

```bash
rm -rf build/_deps/sdl3-build build/_deps/sdl3-src
cmake -B build -S .
cmake --build build -j
```

Do **not** pass `-DPP_BROWSER_HEADLESS=ON` unless you only need compile-only CI builds.

Requires `DISPLAY` (or Wayland session) and X11 dev packages on Linux.

## Tests

```bash
ctest --test-dir build --output-on-failure
```
