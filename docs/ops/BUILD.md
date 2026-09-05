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

**Vendored source** under [`third_party/`](../../third_party/): curl, SQLite (amalgamation), Opus, BoringSSL, Asio, and PeerId/wire helpers (fmt, qtils, soralog, …). JSON (`Value`/`Object`) comes from [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common); libsodium + ML-KEM/ML-DSA from [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto); RmlUi + FreeType/HarfBuzz/LunaSVG + SDL3/SDL3_image from [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui) (sibling or FetchContent).

**System packages:** Linux GUI (X11/GL) + voice (`libpulse-dev` + `libasound2-dev`) + optional video (`libva-dev`). Windows/macOS/mobile use OS audio/video stacks — see Prerequisites table above and [PLATFORMS.md § A/V media](../architecture/PLATFORMS.md#av-media-sdl--calls).

RmlUi is **hard-forked** in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui). libp2p is **hard-forked** under `src/lib/libp2p/` (not in `third_party/`).

If base `third_party/` trees are missing, run `./scripts/vendor/vendor_import.sh` from the repo root.

Chat/CJK fonts (Noto Sans CJK Regular + **Noto Color Emoji** CBDT, with monochrome Noto Emoji as secondary fallback) ship under `assets/fonts/`. FreeType is built with libpng so CBDT color bitmaps load. To refresh fonts:

```bash
./scripts/vendor/fonts_import_noto.sh
```

If libp2p dependency trees are missing, run `./scripts/vendor/libp2p_vendor_import.sh`.

SDL3 + image codecs are vendored in pp-cpp-ui (`third_party/sdl3`, `third_party/sdl3_image`). Bump `PP_CPP_UI_GIT_TAG` when updating them.

## Configure and build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

First-party targets (`src/common`, `src/base`, `src/feature`, `src/app`, and render/libp2p integration glue) compile with `-Wall -Wextra` (MSVC `/W4`) and treat warnings as errors by default (`PP_BROWSER_WARNINGS_AS_ERRORS=ON`). On GCC/Clang, `-Wmissing-field-initializers` and `-Wunused-parameter` are suppressed (noisy with aggregate init and interface overrides). On MSVC, `_CRT_SECURE_NO_WARNINGS`, `/wd4100` (unused parameter), and `/wd4458` (member shadowing; GCC has no `-Wshadow` by default) are applied for the same reasons. Disable the whole policy with `-DPP_BROWSER_WARNINGS_AS_ERRORS=OFF` if you need a temporary escape hatch. Vendored `third_party/` and the RmlUi / libp2p forks keep their own warning settings.

Configure should print `pp-browser: SDL audio backends — PulseAudio + ALSA (dev packages found)` on Linux when voice deps are installed. If you install `libpulse-dev` / `libasound2-dev` after an older configure, wipe the SDL build tree and reconfigure so drivers are not stuck on dummy:

```bash
rm -rf build/_deps/pp_cpp_ui-build/third_party/sdl3
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Owned forks (RmlUi / libp2p)

Use only these `PP_BROWSER_*` knobs. Do not pass raw fork cache vars (`RMLUI_*`, `PACKAGE_MANAGER`, …) — product profiles under `src/lib/pp_lib_*.cmake` set those.

| Option | Default (desktop) | Effect |
|--------|-------------------|--------|
| `PP_BROWSER_BUILD_TESTS` | ON | Host unit/integration tests and in-tree RmlUi unit tests |

Mobile builds default host tests OFF. See [RMLUI_UPSTREAM.md](../architecture/RMLUI_UPSTREAM.md) and [LIBP2P_UPSTREAM.md](../architecture/LIBP2P_UPSTREAM.md).

### libp2p fork (A017 PeerId only)

The in-tree libp2p fork retains PeerId + key wire only (`p2p_peer_id`, `p2p_wire`). Mesh underlay is Amp — see [adp](../../projects/adp/).

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

**Production / packaging** uses a dedicated headless configure (no X11, SDL, or RmlUi):

```bash
cmake -B build-pp-node -S . -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DPP_BROWSER_HEADLESS=ON \
  -DPP_BROWSER_BUILD_TESTS=OFF
cmake --build build-pp-node -j --target pp-node
```

Or: `scripts/platform/pp_node_package_linux.sh configure` (same flags). Linux build host needs only toolchain + `pkg-config` — **not** `libx11-dev` / `libdbus-1-dev` / OpenGL.

**Desktop GUI trees** also produce **`pp-node`** as an extra target (links the same node runtime; GUI deps still configured):

```bash
cmake --build build -j --target pp-node
PP_NODE_AMP_UDP_PORT=18517 ./build/src/app/node/pp-node --pin "$PP_BROWSER_PIN"
```

- PIN via `--pin` or `PP_BROWSER_PIN` (required). Deploy overlays (`PP_NODE_AMP_UDP_PORT`, `PP_NODE_DATA_DIR`, caps, …) follow **CLI → env → config file**; see [CONFIGURATION.md](CONFIGURATION.md#pp-node-deploy-overlays).
- Amp UDP listen is controlled by `libp2p.amp_udp_port` / `PP_NODE_AMP_UDP_PORT` (0 = ephemeral; org seed should pin **443**).
- Dial-back protocol `/pp-browser/dial-back/1.0.0` is enabled for reachability probes (feeds phase **nr**).
- **Status HTTP** (long-running mode): default `127.0.0.1:18518` with `GET /healthz` and `GET /status` (JSON). Separate from the Amp UDP listen port. For console / probes, set `PP_NODE_STATUS_ADDR=0.0.0.0:18518` (ADDR alone is enough) and publish host port **18518**:
  ```bash
  curl -sS http://127.0.0.1:18518/healthz
  curl -sS http://127.0.0.1:18518/status
  ```
  - `--status-addr host:port` or `PP_NODE_STATUS_ADDR` (empty disables; default loopback)
  - `--status-token` / `PP_NODE_STATUS_TOKEN` optional Bearer auth (gates both endpoints)
  - One-shot `pp-node --status` still prints reachability JSON and exits (unchanged)
- Sketches / release packaging: [`packaging/pp-node/`](../../packaging/pp-node/), [`scripts/platform/pp_node_package_linux.sh`](../../scripts/platform/pp_node_package_linux.sh).
- **Image smoke (L0/L1/L2 N-FANOUT):** [`packaging/pp-node/IMAGE_SMOKE.md`](../../packaging/pp-node/IMAGE_SMOKE.md). Local lifecycle: [`scripts/test/pp_local_test.sh`](../../scripts/test/pp_local_test.sh) (`run --suite node|cap|soak|chaos|call-hop|msg-call-hop|mix` / `stop` / `clear`). Loopback thin client (no Docker): `--suite call|conflict|msg-call`. Thin smokes: `pp_node_image_smoke.sh`, `pp_node_relay_smoke.sh`, `pp_node_fanout_smoke.sh`, `pp-node-probe`. Strategy / purpose IDs: [`TEST_STRATEGY.md`](TEST_STRATEGY.md). Doctrine (tiers / layers): [`TESTING.md`](../architecture/TESTING.md).
- **Release trains:** app (`v*` / [`release.yml`](../../.github/workflows/release.yml)) and node (`pp-node/v*` / [`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml)) are independent; cut tags from **`main`**. Tip development is on **`develop`**. See [RELEASE.md](RELEASE.md).
- **Node release CI** builds on Ubuntu 24.04 (same family as `ubuntu:24.04` image), attaches a Linux tarball to the **pp-node** GitHub Release, pushes GHCR, and runs L0 smoke.
- Local tarball + image smoke (on Ubuntu 24.04):

```bash
sudo apt-get install -y cmake ninja-build ccache pkg-config
PP_BROWSER_RELEASE_VERSION=0.0.0-local bash scripts/platform/pp_node_package_linux.sh all
./scripts/test/pp_local_test.sh run --suite node   # hop + L0/L1/N-FANOUT/N-CAP N=4
# or manual compose + ./scripts/test/pp_node_relay_smoke.sh
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

First launch uses Brief defaults (`https://www.brief.global/api/llm/v1`, model `xai` — Brief provider id; upstream Grok version is configured on the Brief AI side). **Register your identity** in Me → Profile — finish registration automatically issues a Brief API key (stored in the profile vault). Until registered, switch to **Cloud** / **Custom** with a key or **Ollama**. Use **Renew registration** near expiry (or with auto-renew on). Rotate the key anytime with **Rotate Brief API key** under Profile while registration is active.

Override the default model with `PP_BROWSER_LLM_MODEL` when no config file exists.

### Sandbox backend (dogfood)

Point relay, directory, registration, LLM, and promoted MCP at the PeoplePost sandbox (`https://www-en.qa.peoplepost.org`) and keep profile data separate from production:

```bash
pp-browser --sandbox
# or: PP_BROWSER_SANDBOX=1 pp-browser
```

Uses config/data under `pp-browser-sandbox` (e.g. `~/.config/pp-browser-sandbox`, `~/.local/share/pp-browser-sandbox` on Linux). Register a **new identity** on the sandbox — production relay IDs and Brief API keys do not carry over. Stale `www.brief.global` URLs in an existing sandbox config file are rewritten on load.

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

Do **not** pass `-DPP_BROWSER_HEADLESS=ON` for the GUI app — that option is for **pp-node** / node-only configures (skips SDL, RmlUi, X11). See [Headless mesh node](#headless-mesh-node-pp-node).

Requires `DISPLAY` (or Wayland session) and X11 dev packages on Linux.

## Lint (include / ifdef policy)

Needs [ripgrep](https://github.com/BurntSushi/ripgrep) (`rg`). On Debian/Ubuntu: `sudo apt install ripgrep`.

```bash
./scripts/check/check_feature_includes.sh
./scripts/check/check_platform_ifdefs.sh
```

OS `#ifdef`s belong in `src/foundation/platform/` or dedicated `*_Win32` / `*_Android` backends — see [PLATFORM_CODE.md](../architecture/PLATFORM_CODE.md). CI runs both scripts on every PR.

## Tests

```bash
ctest --test-dir build --output-on-failure
```

pp-browser tests use a hybrid layout:

- RmlUi fork unit tests (doctest) live in **pp-cpp-ui** and run there with `-DPP_UI_BUILD_TESTS=ON` (not in this repo’s ctest).
- GoogleTest module suites under `src/.../tests/`.

All suites are discovered through CTest. **Writing new tests:** temp SQLite dirs must use a gtest fixture and close stores before `remove_all` — Windows CI fails otherwise; see [TEST_STRATEGY.md § Unit test conventions](TEST_STRATEGY.md#unit-test-conventions).

To run RmlUi fork tests (from the pp-cpp-ui checkout):

```bash
cmake -S . -B build -DPP_UI_BUILD_TESTS=ON
cmake --build build --target rmlui_unit_tests
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
- Optional: Android emulator (API 34+, x86_64 image recommended on Linux hosts)

Set environment variables:

```bash
export ANDROID_SDK_ROOT=/path/to/android/sdk
export ANDROID_NDK_HOME=/path/to/android/ndk
```

### Build and install

```bash
./scripts/platform/android_build.sh apk          # assembleDebug
./scripts/platform/android_build.sh apk-release  # assembleRelease (unsigned; debug keystore)
./scripts/platform/android_build.sh install      # installDebug (requires adb device/emulator)
```

For tagged releases, pass version metadata to CMake:

```bash
export PP_BROWSER_VERSION=0.1.0
export PP_BROWSER_RELEASE_VERSION=0.1.0-rc1
./scripts/platform/android_build.sh apk-release
```

The first clean NDK build can take 15–30 minutes (libp2p + RmlUi + BoringSSL). Assets from [`assets/`](../../assets/) are packaged into the APK automatically.

Launch **pp-browser** on the device/emulator. On first launch, open **Me → Assistant** and enter a cloud API key. Use `adb logcat -s pp-browser` for native logs. Cold-start timing (`[startup]` INFO lines): `adb shell am start -n dev.pp_browser.app/.MainActivity --ez startup_timing true` (or desktop `./pp-browser --startup-timing` / `PP_BROWSER_STARTUP_TIMING=1`). `--debug` also shows them (full DEBUG verbosity).

See [PLATFORMS.md](../architecture/PLATFORMS.md) for mobile lifecycle, navigation, and asset I/O.

## iOS (local, macOS required)

Build **PP.app** for the iOS Simulator or device with CMake + Xcode. Signing placeholders mirror macOS ([`packaging/ios/signing.env.example`](../../packaging/ios/signing.env.example)).

### Prerequisites

- macOS with Xcode 15+ and iOS SDK
- CMake 3.24+, Ninja (recommended)

### Build and run (simulator)

```bash
./scripts/platform/ios_build.sh sim
./scripts/platform/ios_build.sh run-sim
```

Device builds and code signing: [IOS_BUILD.md](IOS_BUILD.md).

The first clean iOS build can take 15–30 minutes (libp2p + RmlUi + BoringSSL). Assets from [`assets/`](../../assets/) are copied into `PP.app/assets/` automatically.
