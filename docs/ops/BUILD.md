# Building pp-browser

**Tier:** ops

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 13+, Clang 16+, or MSVC 2022)
- OpenGL 3.3 drivers
- Linux: `libx11-dev`, `libxext-dev`, `libxcursor-dev`, `libxinerama-dev`, `libxi-dev`, `libxrandr-dev`, `libxfixes-dev`, `libgl-dev`
- Perl (for lsquic code generation)

curl uses vendored **BoringSSL** instead of system `libssl-dev` on Linux.

## Dependencies

**Vendored source** under [`third_party/`](../../third_party/): FreeType, HarfBuzz, nlohmann-json, curl, SDL3, SDL3_image, SQLite (amalgamation), libsodium, and (for libp2p) BoringSSL, Boost, Protobuf, lsquic, and related packages.

**System packages:** X11 and OpenGL development headers on Linux for the GUI.

RmlUi is **hard-forked** under `src/render/fork/`. libp2p is **hard-forked** under `src/libp2p/fork/` (not in `third_party/`).

If base `third_party/` trees are missing, run `./scripts/vendor_import.sh` from the repo root.

Chat/CJK fonts (Noto Sans CJK Regular + Noto Emoji) ship under `assets/fonts/`. To refresh them:

```bash
./scripts/fonts_import_noto.sh
```

If libp2p dependency trees are missing, run `./scripts/libp2p_vendor_import.sh`.

Codec sources under `third_party/sdl3_image/external/` are committed as **regular files** (not git submodules). If configure reports missing externals after clone, re-run `./scripts/vendor_import.sh` and ensure those directories contain source files, not empty gitlink placeholders.

## Configure and build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### libp2p tests and coverage

By default, desktop builds enable in-tree libp2p unit tests (`PP_BROWSER_LIBP2P_TESTING=ON`). Examples are opt-in.

```bash
# Default: pp-browser + libp2p unit tests
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build

# libp2p examples
cmake -B build -S . -DPP_BROWSER_LIBP2P_EXAMPLES=ON
cmake --build build -j

# libp2p coverage (requires gcovr)
cmake -B build -S . -DPP_BROWSER_LIBP2P_COVERAGE=ON -DPP_BROWSER_LIBP2P_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target ctest_coverage_html
```

### Compiler cache (optional)

Speed up rebuilds with **ccache** on Linux/macOS or **sccache** on Windows (MSVC):

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -DPP_BROWSER_COMPILER_CACHE=ON
cmake --build build -j
```

Install the tool first:

- **Linux:** `sudo apt install ccache` (or your distro equivalent)
- **macOS:** `brew install ccache`
- **Windows (MSVC):** `winget install Mozilla.sccache` or `scoop install sccache`

On Windows, Debug and RelWithDebInfo builds use embedded debug info (`/Z7`) so sccache can cache MSVC compiles. Release builds are unaffected.

CI uses ccache (Linux/macOS) and sccache (Windows) automatically via `-DPP_BROWSER_COMPILER_CACHE=ON`.

## Run

From the repository root (assets path is compile-time `PP_BROWSER_ASSETS_DIR`):

```bash
./build/src/app/pp-browser
```

### Simulated touch (optional dev)

To exercise the mobile touch path on desktop (finger events from mouse, red contact dot overlay):

```bash
cmake -B build -S . -DRMLUI_BACKEND_SIMULATE_TOUCH=ON
cmake --build build -j
```

See [INPUT.md](../ui/INPUT.md) for behavior details.

### Brief LLM (default)

First launch uses Brief defaults (`https://www.brief.global/api/llm/v1`, model `grok-4-1-fast-reasoning`). **Register your identity** in Me → Profile — finish registration automatically issues a Brief API key (stored in the profile vault). Until registered, switch to **Cloud** / **Custom** with a key or **Ollama**. Use **Renew registration** near expiry (or with auto-renew on). Rotate the key anytime with **Rotate Brief API key** under Profile while registration is active.

Override the default model with `PP_BROWSER_LLM_MODEL` when no config file exists.

### Cloud LLM (optional)

Open **Me → Assistant**, choose **Cloud (OpenAI-compatible)**, enter your API key, and save.

### Local Ollama (optional dev)

1. Start Ollama and pull a model, e.g. `ollama pull llama3.2`
2. Open **Me → Assistant** → preset **Ollama (localhost)** → save, or use a config with `http://localhost:11434/v1` (see `_llm_ollama_dev_example` in `config.json.example`)

For other providers, set `base_url`, `model`, and API key in Me → Assistant or config JSON.

Config and data paths: [DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md). During development, delete `~/.local/share/pp-browser` if the on-disk layout changes (no legacy migration).

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

pp-browser tests use a hybrid layout:

- RmlUi fork unit tests (doctest) under [`src/render/fork/Tests/`](../src/render/fork/Tests/); enabled with `PP_BROWSER_BUILD_TESTS`.
- GoogleTest module suites under `src/.../tests/`.

All suites are discovered through CTest. To run RmlUi fork tests:

```bash
ctest --test-dir build -R rmlui_unit_tests --output-on-failure
ctest --test-dir build -R ClickRouting --output-on-failure
```

To run a subset of pp-browser GoogleTest suites by name:

```bash
ctest --test-dir build -R "BindingsManifestTest|TurnPlanTest|ConfigJsonTest|SchemaVersionTest" --output-on-failure
```

## Android (local)

Build a debug APK with the Gradle project under [`android/`](../android/). The native library is built from the repo root [`CMakeLists.txt`](../CMakeLists.txt) via NDK and produces `libmain.so` (SDL convention).

### Prerequisites

- Android SDK (API 35) and NDK r26+
- JDK 17+
- Perl (lsquic codegen — same as desktop)
- Optional: Android emulator (API 34+, x86_64 image recommended on Linux hosts)

Set environment variables:

```bash
export ANDROID_SDK_ROOT=/path/to/android/sdk
export ANDROID_NDK_HOME=/path/to/android/ndk
```

### Build and install

```bash
./scripts/android_build.sh apk          # assembleDebug
./scripts/android_build.sh apk-release  # assembleRelease (unsigned; debug keystore)
./scripts/android_build.sh install      # installDebug (requires adb device/emulator)
```

For tagged releases, pass version metadata to CMake:

```bash
export PP_BROWSER_VERSION=0.1.0
export PP_BROWSER_RELEASE_VERSION=0.1.0-rc1
./scripts/android_build.sh apk-release
```

The first clean NDK build can take 15–30 minutes (libp2p + RmlUi + BoringSSL). Assets from [`assets/`](../../assets/) are packaged into the APK automatically.

Launch **pp-browser** on the device/emulator. On first launch, open **Me → Assistant** and enter a cloud API key. Use `adb logcat -s pp-browser` for native logs. Cold-start timing (`[startup]` INFO lines): `adb shell am start -n dev.pp_browser.app/.MainActivity --ez startup_timing true` (or desktop `./pp-browser --startup-timing` / `PP_BROWSER_STARTUP_TIMING=1`). `--debug` also shows them (full DEBUG verbosity).

See [PLATFORMS.md](../architecture/PLATFORMS.md) for mobile lifecycle, navigation, and asset I/O.

## iOS (local, macOS required)

Build **Frame.app** for the iOS Simulator or device with CMake + Xcode. Signing placeholders mirror macOS ([`packaging/ios/signing.env.example`](../../packaging/ios/signing.env.example)).

### Prerequisites

- macOS with Xcode 15+ and iOS SDK
- CMake 3.24+, Ninja (recommended)
- Perl (lsquic codegen — same as desktop)

### Build and run (simulator)

```bash
./scripts/ios_build.sh sim
./scripts/ios_build.sh run-sim
```

Device builds and code signing: [IOS_BUILD.md](IOS_BUILD.md).

The first clean iOS build can take 15–30 minutes (libp2p + RmlUi + BoringSSL). Assets from [`assets/`](../../assets/) are copied into `Frame.app/assets/` automatically.
