# Building pp-browser

**Tier:** ops

## Prerequisites

- CMake 3.24+
- C++20 compiler (GCC 13+, Clang 16+, or MSVC 2022)
- OpenGL 3.3 drivers
- Linux GUI: `libx11-dev`, `libxext-dev`, `libxcursor-dev`, `libxinerama-dev`, `libxi-dev`, `libxrandr-dev`, `libxfixes-dev`, `libgl-dev`, `libdbus-1-dev` (local desktop notifications)
- Linux **voice calls** (a2+): `libpulse-dev`, `libasound2-dev` — SDL3 PulseAudio + ALSA drivers for mic/speaker. Install **both**; without them SDL builds dummy audio only. PipeWire desktops still use `libpulse-dev` (Pulse compatibility).
- Linux **video calls** (a3+, best-effort VA-API): `libva-dev` — soft link at configure time. Without it, `VideoCodec_Linux` builds the unavailable stub (voice still works). Runtime also needs a VA driver package (e.g. `mesa-va-drivers`, `intel-media-va-driver`, or vendor NVIDIA VA support) and a usable `/dev/dri/renderD*`.
- **Windows / macOS voice:** no extra packages — SDL uses **WASAPI** / **CoreAudio**. Ensure OS mic privacy allows the app (Windows Settings → Privacy → Microphone; macOS TCC prompt / shipped apps need mic usage string when notarized).
- **Android / iOS voice:** no Pulse/ALSA packages; SDL uses AAudio/OpenSL ES or CoreAudio via the SDK. **Permissions and audio session are still TODO** before claiming mobile voice — see [PLATFORMS.md § A/V media](../architecture/PLATFORMS.md#av-media-sdl--calls).
- Perl (for lsquic code generation)

Debian/Ubuntu one-liner:

```bash
sudo apt install \
  libx11-dev libxext-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev libxfixes-dev \
  libgl-dev libdbus-1-dev \
  libpulse-dev libasound2-dev \
  libva-dev
```

### Voice / video audio by platform (agents)

| Platform | Build-time audio | Extra install? | Runtime still needed for product voice |
|----------|------------------|----------------|----------------------------------------|
| Linux | SDL Pulse + ALSA | **Yes** — `libpulse-dev` + `libasound2-dev` | Pulse/PipeWire running |
| Windows | SDL WASAPI | No | OS mic privacy if blocked |
| macOS | SDL CoreAudio | No | TCC / `NSMicrophoneUsageDescription` for shipped builds |
| Android | SDL AAudio / OpenSL ES | No (NDK) | `RECORD_AUDIO` (+ runtime grant); later camera |
| iOS | SDL CoreAudio | No (Xcode SDK) | `NSMicrophoneUsageDescription`, `NSCameraUsageDescription`, `AVAudioSession` play-and-record; `UIBackgroundModes` `audio` (a3 wiring) |

### Linux H264 (VA-API) by agents

Configure should print `pp-browser: Linux H264 — libva + libva-drm (VA-API)` when `libva-dev` is present. Missing packages only warn; the codec falls back to the unavailable stub (V017/V019). Hosts without a usable HW encoder may still decode when VLD exists; video **send** can fail (accepted).

curl uses vendored **BoringSSL** instead of system `libssl-dev` on Linux.

## Dependencies

**Vendored source** under [`third_party/`](../../third_party/): FreeType, HarfBuzz, nlohmann-json, curl, SDL3, SDL3_image, SQLite (amalgamation), libsodium, and (for libp2p) BoringSSL, Boost, lsquic, and related packages.

**System packages:** Linux GUI (X11/GL) + voice (`libpulse-dev` + `libasound2-dev`) + optional video (`libva-dev`). Windows/macOS/mobile use OS audio/video stacks — see Prerequisites table above and [PLATFORMS.md § A/V media](../architecture/PLATFORMS.md#av-media-sdl--calls).

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

Configure should print `pp-browser: SDL audio backends — PulseAudio + ALSA (dev packages found)` on Linux when voice deps are installed. If you install `libpulse-dev` / `libasound2-dev` after an older configure, wipe the SDL build tree and reconfigure so drivers are not stuck on dummy:

```bash
rm -rf build/third_party/sdl3
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

### Headless mesh node (`pp-node`)

Desktop builds also produce **`pp-node`** — org/seed daemon without SDL/RmlUi ([p2p-mesh N011](../../projects/p2p-mesh/DECISIONS.md)):

```bash
cmake --build build -j --target pp-node
./build/src/app/node/pp-node --listen /ip4/0.0.0.0/tcp/18517 --pin "$PP_BROWSER_PIN"
```

- PIN via `--pin` or `PP_BROWSER_PIN` (required). Deploy overlays (`PP_NODE_LISTEN`, `PP_NODE_DATA_DIR`, caps, …) follow **CLI → env → config file**; see [CONFIGURATION.md](CONFIGURATION.md#pp-node-deploy-overlays).
- Default listen is fail-loud on the configured port (often **443** for ops). Pass `--listen-fallback` / `PP_NODE_LISTEN_FALLBACK=1` only for local dogfood.
- Dial-back protocol `/pp-browser/dial-back/1.0.0` is enabled for reachability probes (feeds phase **nr**).
- **Status HTTP** (long-running mode): default `127.0.0.1:18518` with `GET /healthz` and `GET /status` (JSON). Separate from the libp2p listen port. For console / probes, set `PP_NODE_STATUS_ADDR=0.0.0.0:18518` (ADDR alone is enough) and publish host port **18518**:
  ```bash
  curl -sS http://127.0.0.1:18518/healthz
  curl -sS http://127.0.0.1:18518/status
  ```
  - `--status-addr host:port` or `PP_NODE_STATUS_ADDR` (empty disables; default loopback)
  - `--status-token` / `PP_NODE_STATUS_TOKEN` optional Bearer auth (gates both endpoints)
  - One-shot `pp-node --status` still prints reachability JSON and exits (unchanged)
- Sketches / release packaging: [`packaging/pp-node/`](../../packaging/pp-node/), [`scripts/pp_node_package_linux.sh`](../../scripts/pp_node_package_linux.sh).
- **Release CI** builds `pp-node` on **Ubuntu 24.04** (same family as the runtime image `ubuntu:24.04`), attaches `pp-node-<ver>-linux-amd64.tar.gz` to the GitHub Release, and pushes `ghcr.io/<owner>/pp-node:<ver>`. See [RELEASE.md](RELEASE.md).
- Local tarball + image smoke (on Ubuntu 24.04):

```bash
sudo apt-get install -y cmake ninja-build ccache pkg-config
PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/pp_node_package_linux.sh all
docker build -t pp-node:local dist/pp-node/docker
```

  Keep build host and image on the same Ubuntu 24.04 family so glibc matches.

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
