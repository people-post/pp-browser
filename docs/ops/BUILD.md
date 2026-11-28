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

curl uses **BoringSSL** from [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto) instead of system `libssl-dev` on Linux, Android and iOS (Windows: Schannel; macOS: SecureTransport). pp-cpp-crypto's XChaCha20-Poly1305 (Amp's link AEAD, the message cipher) seals with it on every platform; Windows x86 builds need NASM for its assembly.

## Dependencies

**Vendored source** under [`third_party/`](../../third_party/), its one home ([third_party/README.md](../../third_party/README.md), manifest [`UPSTREAM.json`](../../third_party/UPSTREAM.json): name, version, licence, upstream for each): curl, SQLite (amalgamation), Opus, qrcodegen, quirc, zlib, Asio, googletest. Never fetched at configure time.

**People-post libraries**, each at a release tag of the **pin list** [`cmake/PpPins.cmake`](../../cmake/PpPins.cmake) ([below](#pins-and-sibling-builds)): JSON (`Value`/`Object`) and the runtime from [`pp-cpp-common`](https://github.com/people-post/pp-cpp-common); libsodium + ML-KEM/ML-DSA + BoringSSL from [`pp-cpp-crypto`](https://github.com/people-post/pp-cpp-crypto); Amp links from [`pp-cpp-amp`](https://github.com/people-post/pp-cpp-amp); the mesh (host, reachability, DHT, directory, relays) from [`pp-cpp-mesh`](https://github.com/people-post/pp-cpp-mesh); RmlUi + FreeType/HarfBuzz/LunaSVG + SDL3/SDL3_image from [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui); pp-node's ledger role from [`pp-ledger`](https://github.com/people-post/pp-ledger).

**System packages:** Linux GUI (X11/GL) + voice (`libpulse-dev` + `libasound2-dev`) + optional video (`libva-dev`). Windows/macOS/mobile use OS audio/video stacks — see Prerequisites table above and [PLATFORMS.md § A/V media](../architecture/PLATFORMS.md#av-media-sdl--calls).

RmlUi is **hard-forked** in [`pp-cpp-ui`](https://github.com/people-post/pp-cpp-ui). The in-tree libp2p fork is retired ([LIBP2P_UPSTREAM.md](../architecture/LIBP2P_UPSTREAM.md)).

If base `third_party/` trees are missing, run `./scripts/vendor/vendor_import.sh` from the repo root.

Chat/CJK fonts (**Noto Sans CJK Regular OTC** `NotoSansCJK-Regular.ttc` — shared JP/KR/SC/TC/HK faces — + **Noto Color Emoji** CBDT, with monochrome Noto Emoji as secondary fallback) ship under `assets/fonts/`. The OTC is loaded once into memory and registered with per-region `face_index` values. FreeType is built with libpng so CBDT color bitmaps load. Simplified Chinese is one fallback family in three weights on every platform — the OTC's SC face (400) plus `NotoSansSC-Medium.otf` (500) and `NotoSansSC-Bold.otf` (700) — so Chinese in medium / bold text gets a heavier face (the engine picks the fallback face nearest to the text's weight). **Inter** (`Inter-{Regular,Medium,SemiBold,Bold}.ttf`, rsms/inter v4.1) is registered as family `Inter`; the default family is still `LatoLatin`. To refresh fonts:

```bash
./scripts/vendor/fonts_import_noto.sh
```

If libp2p dependency trees are missing, run `./scripts/vendor/libp2p_vendor_import.sh`.

SDL3 + image codecs are vendored in pp-cpp-ui (`third_party/sdl3`, `third_party/sdl3_image`). Bump the pp-cpp-ui pin when updating them (and, for a new SDL version, SDL's Java glue under `android/`: third_party/README.md § Elsewhere in this repo).

### Pins and sibling builds

**One pin list** ([`cmake/PpPins.cmake`](../../cmake/PpPins.cmake), [capability-repos CR012](../../projects/capability-repos/DECISIONS.md#cr012--before-any-repo-moves-pins-vendored-deps-visibility)): every people-post library pp-browser and pp-node build against, one `pp_pin(<repo> TAG <tag>)` line each. A library bump is one line there (during release 1's freeze the pins stay put). The `cmake/Pp<Lib>.cmake` files add each library with its own options and hold no tag; the mechanism is [`cmake/PpFetchPin.cmake`](../../cmake/PpFetchPin.cmake), which another repo with its own list (a capabilities repo, pp-ledger-apps) copies.

Each pin `<repo>` has cache variables named after it (`pp-cpp-mesh` → `PP_CPP_MESH_…`); a library comes from the first of:

| Source | How | When |
|--------|-----|------|
| A local checkout of one library | `-DPP_CPP_MESH_SOURCE_DIR=<path>` (any pin: `PP_CPP_COMMON_`, `PP_CPP_CRYPTO_`, `PP_CPP_AMP_`, `PP_CPP_MESH_`, `PP_CPP_UI_`, `PP_LEDGER_SOURCE_DIR`) | Working on that library with the app |
| **Sibling mode**: every library from a checkout beside the repo | `-DPP_SIBLINGS_DIR=..` (relative to the repo root, or absolute): `<dir>/pp-cpp-common`, `<dir>/pp-cpp-mesh`, …; a pin with no checkout there is fetched at its tag (warned) | A change across libraries and the app, built together before any release |
| The pinned tag (FetchContent) | default; `-DPP_CPP_MESH_GIT_TAG=<tag>` tries another tag for one build | CI, releases, everyday builds |

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DPP_SIBLINGS_DIR=..   # ../pp-cpp-common, ../pp-cpp-mesh, …
```

**pp-specs** is pinned the same way but only fetched when tests are built, and nothing in it is compiled: it holds the
shared contracts' frozen vectors, which the tests read from `PP_SPECS_VECTORS_DIR` (`cmake/PpSpecs.cmake`;
`-DPP_SPECS_SOURCE_DIR=<path>` or sibling mode for a local checkout). A check that reads them without a configure
(`check_relay_ids.sh --self-test`) finds them through `PP_SPECS_DIR`, a sibling `../pp-specs` or a build's fetched copy;
`scripts/dev/pp_specs_at_pin.sh <dir>` clones the pinned tag (CI's lint job does).

A local checkout (either way) is checked at configure: when its HEAD is not the pinned tag, CMake warns, saying whether it is past the pin (it has library changes CI's build does not) or does not contain it (update it, or the build differs from CI's); with no such tag in the checkout, it asks for `git fetch --tags`. Configure prints where each library came from (`pp-browser: pp-cpp-mesh from … (sibling; pin v10.1.0)`). A packaged build (`PP_BROWSER_PACKAGED_BUILD=ON`) refuses sibling mode: a release compiles its pinned tags (PB-001). Nothing beside the repo is picked up unasked — `../pp-cpp-ui` was until CR012; configure says so once when one is there.

A pin bump follows a tag (`pp_fetch_git_tag`): an existing build tree takes the new pin at its next configure, unless that tree overrides the tag with `-D…_GIT_TAG`.

## Configure and build

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

First-party targets (everything under `capabilities/*/src` and `src/`, plus integration glue) compile with `-Wall -Wextra` (MSVC `/W4`) and treat warnings as errors by default (`PP_BROWSER_WARNINGS_AS_ERRORS=ON`). On GCC/Clang, `-Wmissing-field-initializers` and `-Wunused-parameter` are suppressed (noisy with aggregate init and interface overrides). On MSVC, `_CRT_SECURE_NO_WARNINGS`, `/wd4100` (unused parameter), and `/wd4458` (member shadowing; GCC has no `-Wshadow` by default) are applied for the same reasons. Disable the whole policy with `-DPP_BROWSER_WARNINGS_AS_ERRORS=OFF` if you need a temporary escape hatch, or for an older compiler with false positives (GCC 12, as on Debian 12 and Raspberry Pi OS, flags `std::string` / `std::vector` code under `-Wstringop-overflow` and `-Wrestrict`). The one switch also turns `-Werror` off in pp-cpp-mesh, pp-cpp-ui and pp-ledger (from the pp-ledger release with `PP_LEDGER_WARNINGS_AS_ERRORS`), in a fresh or an existing build folder; the warnings are still shown. Vendored `third_party/` and the RmlUi fork keep their own warning settings (BoringSSL, in pp-cpp-crypto, keeps its own `-Werror`).

Configure should print `pp-browser: SDL audio backends — PulseAudio + ALSA (dev packages found)` on Linux when voice deps are installed. If you install `libpulse-dev` / `libasound2-dev` after an older configure, wipe the SDL build tree and reconfigure so drivers are not stuck on dummy:

```bash
rm -rf build/_deps/pp_cpp_ui-build/third_party/sdl3
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

### Build identity (commit and tag)

A build says what it is ([release-compatibility DESIGN § App builds](../../projects/release-compatibility/DESIGN.md#app-builds-internal-external-store)), so a report names the build it came from:

- **About** shows it under the version ("Build v1.0.0-rc.3 · a1b2c3d4", or "Build a1b2c3d4" without a tag; zh "构建 …"; `FormatBuildLabel`).
- **Diagnostics** name it as `<version> · <tag> · <commit>` ("1.0.0 · v1.0.0-rc.3 · a1b2c3d4", "1.0.0 · a1b2c3d4" without a tag; `FormatBuildIdentity` / `BuildIdentity` in `src/foundation/product/BuildInfo.h`): the first line of a Send logs archive, and `commit=` / `tag=` lines in the crash record and its envelope ([DIAGNOSTICS.md](../contracts/DIAGNOSTICS.md)).
- **Logs**: every app launch logs `app.build version=… commit=… tag=…`, every pp-node start `pp-node.build version=… commit=… tag=…` ([LOGGING.md](../architecture/LOGGING.md#developer-log-vs-shareable-lines)); pp-node's `/metrics` labels `pp_build_info` with `commit` and `tag` ([NODE_METRICS.md](../contracts/NODE_METRICS.md)). What pp-node sends the relay: `node_version` at registration, and `X-PP-Client: pp-node/<version> <os>` on its relay calls ([SERVICE_ENDPOINTS § Version header](../contracts/SERVICE_ENDPOINTS.md#version-header)).

Both values are made on **every build**, not at configure time (`pp_build_info_generate`, CMake script mode in `src/foundation/product/GenBuildInfo.cmake`: no shell, so it is the same on macOS, iOS, Android, Windows and Linux). It writes `BuildInfoGenerated.cpp` into the build tree only when its text changed, so an incremental build recompiles that one file. No timestamps: one commit and tag give the same file. A build with no git and no variables configures and builds (commit `unknown`, no tag).

- **Commit**: `-DPP_BUILD_COMMIT=<hash>`, or the `PP_BUILD_COMMIT` environment variable of the build when the cache variable is empty (a hex hash, cut to 8; anything else is ignored with a warning). Otherwise `git rev-parse --short=8 HEAD`, plus `-dirty` when tracked files have uncommitted changes (`git status --porcelain --untracked-files=no`); `unknown` when git or the repository is not there.
- **Tag** (e.g. `internal/141`): `-DPP_BUILD_TAG=<tag>`, or the `PP_BUILD_TAG` environment variable of the build when the cache variable is empty (so `scripts/platform/*_build.sh` and Gradle pick it up without passing it). Only `A-Za-z0-9._/+-` are kept. Otherwise, on a clean checkout whose commit is HEAD, a build tag on HEAD (`git tag --points-at HEAD`), only one named as [release-compatibility § Builds, tags and deploys](../../projects/release-compatibility/DESIGN.md#builds-tags-and-deploys) names them, first of `vX.Y.Z`, `vX.Y.Z-rc.<n>`, `internal/<n>`, `pp-node/vX.Y.Z`, `pp-node/vX.Y.Z-rc.<n>`, `node-internal/<date>.<n>` (highest version first); any other tag, or a modified tree, means no tag. None: About shows only the commit.
- **CI**: `release.yml` (`v*` tags) passes the tag and `github.sha`; `release-pp-node.yml` passes them for a `pp-node/v*` tag push (a manual run builds `inputs.ref`, so it leaves both to git); `ios-testflight.yml` passes `github.sha` (its `internal/<build number>` tag is the tagging workflow's). `build.yml` builds nobody installs pass nothing.

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

CI uses ccache on every platform automatically via `-DPP_BROWSER_COMPILER_CACHE=ON`. Its Actions cache key includes the runner image (`ImageOS`-`ImageVersion`): ccache misses every object after a compiler change, so a new image starts its own cache, restoring the newest one meanwhile.

### Capabilities on their own

`capabilities/CMakeLists.txt` builds chosen capability folders and what is below them, without the app — the shape of a capability repo (capability-repos [CR023](../../projects/capability-repos/DECISIONS.md#cr023--calls-becomes-repo-ready-first) step 4):

```bash
cmake -S capabilities -B build-calls -DCMAKE_BUILD_TYPE=Debug -DPP_CAPABILITIES=calls   # base … media, calls
cmake --build build-calls --target pp_capability_calls_tests -j                          # or everything
ctest --test-dir build-calls -L '^capability:calls$' -j
```

It reads the same pin list and `third_party/`, and takes the same options (`PP_SIBLINGS_DIR`, `…_SOURCE_DIR`, `PP_BROWSER_COMPILER_CACHE`); `pp_capability_<name>_tests` builds one capability's tests. Linux and macOS desktop. CI's `capabilities` job runs it for calls ([capabilities/calls/README.md](../../capabilities/calls/README.md)).

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

**UDP buffers (host):** a node asks for 8 MiB socket buffers so bursts wait in the kernel instead of being dropped; Linux grants at most `net.core.rmem_max` / `wmem_max` (default 208 KiB). Install [`packaging/pp-node/99-pp-node-udp.conf`](../../packaging/pp-node/99-pp-node-udp.conf) in `/etc/sysctl.d/` (for Docker, on the host); `pp_udp_receive_buffer_bytes` on `/metrics` shows what the node got. Measurements: [STRESS_LAB.md](STRESS_LAB.md).

Or: `scripts/platform/pp_node_package_linux.sh configure` (same flags). Linux build host needs only toolchain + `pkg-config` — **not** `libx11-dev` / `libdbus-1-dev` / OpenGL.

**Desktop GUI trees** also produce **`pp-node`** as an extra target (same node runtime). It must stay **UI-free** even when SDL/RmlUi are configured for `pp-browser`: it links the product base both apps use (`pp_product_platform`, `pp_product_data`, `pp_foundation_product`: capabilities/product, capability-repos CR028) and other `*_core` targets, never the desktop's `pp_foundation_platform(_core)` / `pp_foundation_data` / `pp_gui` / `pp_lib_media`. CMake enforces this via `pp_browser_assert_no_ui_dependencies` + a post-link symbol check in [`apps/node/CMakeLists.txt`](../../apps/node/CMakeLists.txt). Beyond UI-free, its first-party closure is an **allowlist** (`pp_browser_assert_node_link_allowlist`, `PP_BROWSER_NODE_LINK_ALLOWLIST` in [`cmake/PpBrowserNoUiDeps.cmake`](../../cmake/PpBrowserNoUiDeps.cmake)): foundation `*_core`-style libs, `pp_product_{platform,data}`, `pp_pbr_common`, `pp_lib_{people,net}`, headless `pp_domain_{registration,node}`, the node app's own `pp_app_node`, the libraries of the binary's services ([`cmake/PpNodeServices.cmake`](../../cmake/PpNodeServices.cmake); `pp_domain_node_ledger` for the ledger), the Amp / common / crypto / mesh siblings (not `pp_mesh_testing`), and pp-ledger. Any other `pp_*` target fails configure — widening the node is a deliberate edit to that list.

**pp-node on its own** (capability-repos CR028): its folder, [`apps/node/`](../../apps/node/), is a root of its own too. It builds pp-node, pp-node-plus and their tests without `src/`, the capabilities it links from `capabilities/` (headless, the same pins), and CI's `node-editions` job keeps it so:

```bash
cmake -S apps/node -B build-node -DCMAKE_BUILD_TYPE=Release
cmake --build build-node -j
ctest --test-dir build-node -j
```

The app's configure adds the same folder, its binaries under `build/src/app/node/` as before.

**Two tiers** (projects/node-relay NR001). **`pp-node`** is the core every node runs: it helps the mesh (circuit and media relay, DHT, directory) and holds nothing for anyone. The ledger role is part of the core, and it runs only when asked (`--ledger-role`); `PP_BROWSER_WITH_LEDGER=OFF` builds without it. **`pp-node-plus`** is pp-node plus the **add-on services**: services that keep data or act for accounts and that an owner offers. The first is the message relay (the `relay` capability, `capabilities/relay/`: its settings and store so far, off until `node.relay.enabled` ([CONFIGURATION § pp-node services](CONFIGURATION.md#pp-node-services-node)); NR3 adds the protocol); file storage comes later (NR010). Each service has one folder of its own: `capabilities/<service>/` in the tree, `node.<service>` in the config, `<profile dir>/<service>/` on disk. **Services are chosen at compile time** ([`cmake/PpNodeServices.cmake`](../../cmake/PpNodeServices.cmake)): `-DPP_NODE_SERVICES=…` for pp-node (default the `node` preset, `ledger`) and `-DPP_NODE_PLUS_SERVICES=…` for pp-node-plus (default `plus`, `ledger;relay`). Any set builds; a service that needs another fails configure without it. Nothing on the wire, on disk or in the config names a set, so a service can move between the presets (NR001). Both binaries are built from the same sources, commit and version, each with its own image. Configure once and build both (`--target pp-node-plus`; a desktop tree builds it only when asked). `/status` says which set answers (`edition`: `node`, `plus` or `custom`; `services`; `dormant`, the services this build lacks whose data is in the profile, kept and not served), and so does the `pp-node.build` log line. Builds may share a data folder one at a time, never at once (NR001's four rules). The default node stays small, and CI's `node-editions` job checks it on every PR that touches what it is built from:

- **Each binary links exactly its set.** Each service is a capability with its own library, code in its own namespace (`pbr::<service>`) and `PP_NODE_HAS_<SERVICE>` on its library, which starts it in the binary that links it. A binary's link allowlist adds only its set's libraries (`EXTRA`). A post-link symbol check (`pp_browser_add_symbol_check`, [`cmake/CheckSymbols.cmake`](../../cmake/CheckSymbols.cmake)) fails a binary that has a service outside its set, or lacks one in it. CI also builds the default node with no service and with each alone. It runs on the unstripped binary and needs `nm` (not MSVC).
- **Its size.** [`scripts/check/check_node_size.sh`](../../scripts/check/check_node_size.sh) holds the stripped binary (14 MiB) and what the image adds to `ubuntu:24.04` (18 MiB) to budgets. Raise one only in the PR that needs it, and say why there.
- Both images are built (not pushed) and pass the L0 smoke (`pp_node_image_smoke.sh --expect-edition node|plus --expect-services …`). A `pp-node-plus` release is NR5's.

```bash
scripts/platform/pp_node_package_linux.sh all                                   # pp-node → dist/pp-node/
PP_NODE_BINARY=pp-node-plus scripts/platform/pp_node_package_linux.sh build     # same tree
PP_NODE_BINARY=pp-node-plus scripts/platform/pp_node_package_linux.sh package   # → dist/pp-node-plus/
PP_NODE_SERVICES="ledger;relay" scripts/platform/pp_node_package_linux.sh all    # a custom pp-node
scripts/check/check_node_size.sh dist/pp-node/docker/pp-node [<pp-node image>]
```

```bash
cmake --build build -j --target pp-node
PP_NODE_AMP_UDP_PORT=18517 ./build/src/app/node/pp-node --pin "$PP_BROWSER_PIN"
```

- Config: `--config` / `PP_BROWSER_CONFIG` only (packaging passes `/etc/pp-node/config.json`); without one the node runs on defaults and env — it never reads the desktop app's config of the same user. Logs at `info` (`--log-level` / `PP_NODE_LOG_LEVEL`; `--debug`).
- PIN via `--pin` or `PP_BROWSER_PIN` (required). Deploy overlays (`PP_NODE_AMP_UDP_PORT`, `PP_NODE_DATA_DIR`, caps, …) follow **CLI → env → config file**; see [CONFIGURATION.md](CONFIGURATION.md#pp-node-deploy-overlays).
- Amp UDP listen is controlled by `mesh.amp_udp_port` / `PP_NODE_AMP_UDP_PORT` (0 = ephemeral; org seed should pin **443**).
- Dial-back protocol `/pp-mesh/reach/1.0.0` is enabled for reachability probes (feeds phase **nr**).
- **Status HTTP** (long-running mode): default `127.0.0.1:18518` with `GET /healthz` and `GET /status` (JSON), and `GET /metrics` (Prometheus text for operator scraping — [NODE_METRICS.md](../contracts/NODE_METRICS.md)). Separate from the Amp UDP listen port. For console / probes, set `PP_NODE_STATUS_ADDR=0.0.0.0:18518` (ADDR alone is enough) and publish host port **18518**:
  ```bash
  curl -sS http://127.0.0.1:18518/healthz
  curl -sS http://127.0.0.1:18518/status
  curl -sS http://127.0.0.1:18518/metrics
  ```
  - `--status-addr host:port` or `PP_NODE_STATUS_ADDR` (empty disables; default loopback)
  - `--status-token` / `PP_NODE_STATUS_TOKEN` optional Bearer auth (gates both endpoints)
  - One-shot `pp-node --status` still prints reachability JSON and exits. It boots the node's own identity, so while a node runs on the same data dir it refuses (next point); ask the running node with `curl …/status` instead
- **One node per data folder at a time** (projects/node-relay NR001): a node holds `<data dir>/pp-node.lock` (POSIX `flock`, Windows `LockFileEx`) for its life, and a second process on the same folder — another node, or `--status` — exits at once with `another pp-node is running on <data dir>`. Two would run one identity twice on the mesh and write the same sealed files. The OS drops the lock when the process ends, a crash or `kill -9` included, so there is never a stale lock to clear. Different builds may use the same folder in turn, one at a time
- **Ledger role** (pp-ledger platform-integration pi4): `--ledger-role relay|miner` (or `PP_NODE_LEDGER_ROLE`) runs one pp-ledger role on the node's mesh runtime, with the node's identity and address, from `--ledger-dir` (default `<profile data dir>/ledger`: pp-ledger's own `config.json`, keys and data). A first run with no `config.json` writes pp-ledger's template and exits with what to fill in; the role advertises the ledger's protocols while it serves them, and stops before the mesh. Built when `PP_BROWSER_WITH_LEDGER` is on (the default, but not on mobile): pp-ledger comes from [`cmake/PpLedger.cmake`](../../cmake/PpLedger.cmake) (pinned in `cmake/PpPins.cmake`; `-DPP_LEDGER_SOURCE_DIR=…` for a local checkout), linked only by `pp_domain_node_ledger`. Smoke: `pp_local_test.sh run --suite ledger` (N-LEDGER).
- Sketches / release packaging: [`packaging/pp-node/`](../../packaging/pp-node/), [`scripts/platform/pp_node_package_linux.sh`](../../scripts/platform/pp_node_package_linux.sh).
- **Image smoke (L0/L1/L2 N-FANOUT):** [`packaging/pp-node/IMAGE_SMOKE.md`](../../packaging/pp-node/IMAGE_SMOKE.md). Local lifecycle: [`scripts/test/pp_local_test.sh`](../../scripts/test/pp_local_test.sh) (`run --suite node|cap|soak|chaos|call-hop|msg-call-hop|mix` / `stop` / `clear`). Loopback thin client (no Docker): `--suite call|conflict|msg-call`. Thin smokes: `pp_node_image_smoke.sh`, `pp_node_relay_smoke.sh`, `pp_node_fanout_smoke.sh`, `pp-node-probe`. Strategy / purpose IDs: [`TEST_STRATEGY.md`](TEST_STRATEGY.md). Doctrine (tiers / layers): [`TESTING.md`](../architecture/TESTING.md).
- **Release trains:** app (`v*` / [`release.yml`](../../.github/workflows/release.yml)) and node (`pp-node/v*` / [`release-pp-node.yml`](../../.github/workflows/release-pp-node.yml)) are independent; their tags go on release branches cut from **`main`** (`release/app-X.Y`, `release/node-X.Y`). See [RELEASE.md](RELEASE.md).
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

First launch uses Brief defaults (`<origin>/api/llm/v1`, model `xai` — Brief provider id; upstream Grok version is configured on the Brief AI side). **Register your identity** in Me → Profile — finish registration automatically issues a Brief API key (stored in the profile vault). Until registered, switch to **Cloud** / **Custom** with a key or **Ollama**. Use **Renew registration** near expiry (or with auto-renew on). Rotate the key anytime with **Rotate Brief API key** under Profile while registration is active.

Override the default model with `PP_BROWSER_LLM_MODEL` when no config file exists.

### Backend origin

The client ships with no backend origin. Each built-in backend (prod, staging, dev) is a table of services ([profile-model PM002](../../projects/profile-model/DECISIONS.md#pm002--a-backend-is-a-named-table-of-services-each-with-its-own-url); `BackendTable`) which the build fills in from that environment's one origin (relay, directory and registration under `/api/relay`, the Brief LLM and its token service under `/api/llm/v1`, the promoted MCP seed under `/mcp`, crash intake, client-compat and the locker under the relay); a custom backend's table is the user's (CONFIGURATION § backend.services); while it is empty those features are disabled and the app still starts. An origin is `scheme://host[:port]` only: no path (the client appends its own), and a trailing slash is stripped.

- Compile time, up to three origins: `cmake -B build -S . -DPP_API_URL_PROD=https://backend.example.test -DPP_API_URL_STAGING=https://staging.backend.example.test -DPP_API_URL_DEV=https://dev.backend.example.test` — the environments are **dev**, **staging** and **prod** (release-compatibility RC009 as amended; qa is retired, RC021, and staging is a new environment, not qa renamed). `PP_API_URL_STAGING` and `PP_API_URL_DEV` are optional (developer and test builds; RC015: store builds compile prod only); when one is empty that choice is not offered in Me → Network, and a saved `staging` / `dev` (or `qa`) runs as `prod` in a store build (as staging, else dev, in a tester build, below); the saved choice itself stays in the file.
- CI: `PP_API_URL_PROD`, `PP_API_URL_STAGING` and `PP_API_URL_DEV` are repository secrets of the same names. `release.yml` (candidates, and so releases) passes prod only, so a release offers Production and Custom; `ios-testflight.yml` passes staging and dev only, and no prod, to a hand-run tester build (no candidate tag, `PP_TESTER_BUILD=ON`), which offers Staging, Dev and Custom; a release candidate's run passes prod only; `release-pp-node.yml` passes all three; `scripts/platform/ios_build.sh`, `android_build.sh` and `pp_node_package_linux.sh` read the environment variables of those names. A tagged pp-node release fails when `PP_API_URL_PROD` is empty: the node's registration URL derives from it, and a node that publishes itself to the mesh directory (`mesh_publish`, with the opt-in heartbeat) cannot register without one ("mesh_publish requires registration.base_url"). A dogfood build only warns; such a node needs `registration.base_url` in its config or `PP_NODE_REGISTRATION_BASE_URL`.
- The org relays' ids and certificates (release-compatibility [SIGNED_FORMATS § 3.5](../../projects/release-compatibility/SIGNED_FORMATS.md#35-bootstrap): a relay address is its origin and its relay id, and the app never learns it from the relay's own answer): `-DPP_RELAY_ID_PROD=relay:… -DPP_RELAY_ID_STAGING=relay:… -DPP_RELAY_ID_DEV=relay:…` beside the origins — each the relay id that backend's relay proves (its relay link's part after `#`; pp-relay-apis' `scripts/relay-operator.mjs link` prints it, and `GET <origin>/api/relay/v1/relay-identity` names it). The default is the placeholder **`pending`**: that environment's operator set is not made yet (Leo makes each; dev's exists, below), and it **fails closed** — the app and pp-node do not use that relay at all and never pin it on first use. CMake refuses a value that is neither `relay:` and 43 base64url characters, `pending`, nor empty; empty (by hand only) is a dogfood build's: the relay is pinned on its first valid certificate, warned. Beside each, the **certificate at build time**: `config/relay-certs/prod.cert` / `staging.cert` / `dev.cert` (paths `PP_RELAY_CERT_FILE_PROD` / `_STAGING` / `_DEV`), the certificate's standard base64 as `relay-operator.mjs` prints it (lines may wrap; `#` lines are comments), compiled in as the floor a fresh install starts from (Q10: it knows the audience before its first fetch, and never accepts an older one); `prod.cert` is a committed **placeholder** (`pending`) until Leo's certificate 1; `dev.cert` holds dev's certificate 1 (relay id `relay:oIAOO-vlKGWEQvX0WT9pEMZPy4Q50NxmwI0jg9tJjXQ`, operator set 1 of 1, origin `https://api-main.pp.peoplepost.org/api/relay`); `staging.cert` holds staging's certificate 1 (relay id `relay:ptanecM6rEtR0aDrzcaupmlUaVVZGMi_QxYlexCgafs`, operator set 1 of 1, origin `https://api-staging.pp.peoplepost.org/api/relay`). Each new certificate replaces its file in the next build; a new operator set is a new relay id, so its file and its `PP_RELAY_ID_<ENV>` change together. CI: repository **variables** (or secrets) `PP_RELAY_ID_PROD`, `PP_RELAY_ID_STAGING` and `PP_RELAY_ID_DEV`, passed by `release.yml` (prod only), `ios-testflight.yml` (prod for a candidate, staging and dev for a tester build) and `release-pp-node.yml` (`build.yml` passes none, so `pending`) to CMake, `scripts/platform/ios_build.sh`, Gradle (`android/app/build.gradle`) and `pp_node_package_linux.sh` (unset or empty: `pending`). `scripts/check/check_relay_ids.sh release` fails a tagged app release, a release candidate's TestFlight upload and a published pp-node release when `PP_RELAY_ID_PROD` is not a relay id (unset, empty, `pending`, malformed) or `prod.cert` holds no certificate (and the same for staging / dev, with `staging.cert` / `dev.cert`, when the build compiles `PP_API_URL_STAGING` / `PP_API_URL_DEV`); a pp-node dogfood build only warns (`dogfood`). In both modes it fails when a set `PP_RELAY_ID_<ENV>` and its `.cert` disagree (the certificate's founding operator set hashes to another relay id) or the file is not a version-1 certificate it reads in full: the file and the variable were not changed together. It checks the relay id, not the signatures (`relay-operator.mjs verify` does). The lint job runs its `--self-test`. A rotated transport key is a new certificate (a higher sequence), not a new relay id: the next build carries it, and running apps adopt it from `relay-identity` or `client-compat`.
- **Build kind** (release-compatibility [RC022](../../projects/release-compatibility/DECISIONS.md#rc022--tester-builds-never-touch-production-profiles-hard-guard)): `-DPP_TESTER_BUILD=ON` makes a **tester build** — an `internal/<n>` TestFlight upload, a tester APK or desktop tester package. On a phone a tester build and the store app share one app and one data container, so a tester build never offers or opens production: CMake refuses `PP_API_URL_PROD` and a `relay:` `PP_RELAY_ID_PROD` with it (and compiles no `prod.cert`), the app never runs `prod` (a saved `prod`, or a backend it has no origin for or whose `PP_RELAY_ID_<ENV>` is `pending`, runs as the first usable of staging, else dev, else only a custom backend), Me → Network lists no Production nor a staging / dev whose relay id is pending, About shows "Tester build" with its TestFlight build number (the `<n>` of its `internal/<n>` tag, else its version; a store build shows no such line), its custom profiles are `test-custom-<hash>`, and it refuses to open a profile recorded for `prod` or `custom-…` ([DATA_LAYOUT § Per-backend profiles](../contracts/DATA_LAYOUT.md#per-backend-profiles)). Testers who want their prod data reinstall the store app. **OFF (the default)** is a store build — a release or a release candidate (built once and promoted: it is the store binary) — or a developer's local build, which compiles no origin anyway. Who sets it: `ios-testflight.yml` passes `PP_TESTER_BUILD=ON` exactly when it has no `candidate_tag` (the same condition that drops prod and adds staging and dev) and `OFF` for a candidate; `release.yml` passes `-DPP_TESTER_BUILD=OFF` (macOS, Windows) and `PP_TESTER_BUILD=OFF` (Android); `build.yml` and `release-pp-node.yml` pass nothing (OFF: CI checks and their artifacts compile no origin; pp-node is no tester build). `scripts/platform/ios_build.sh` and Gradle (`android/app/build.gradle`, through `android_build.sh`) read `PP_TESTER_BUILD` (`ON` / `OFF`, unset `OFF`) and always pass it, so a reused build directory never keeps an earlier kind; `check_relay_ids.sh` skips prod for a tester build and fails one that is passed prod's origin or relay id. No workflow builds a tester APK or desktop tester package yet: set `PP_TESTER_BUILD=ON` (and staging / dev, no prod) for one.
- Relay content hash: `-DPP_RELAY_CONTENT_SHA256=ON` makes `relay.content_sha256` default to true in that build (the header `x-amz-content-sha256` on relay POST/PUT, required by a relay API behind CloudFront origin access control). Default OFF; `relay.content_sha256` in the config file (or `PP_NODE_RELAY_CONTENT_SHA256` for pp-node) overrides it. App builds in CI (`build.yml` `linux-package`, `release.yml`, `ios-testflight.yml`, via `scripts/platform/*_build.sh`) take it from the repository variable `PP_RELAY_CONTENT_SHA256` (`ON`/`OFF`, unset = OFF). pp-node builds do not pass it; the fleet enables it at runtime with `PP_NODE_RELAY_CONTENT_SHA256`.
- Runtime override: `PP_BROWSER_ORIGIN=https://backend.example.test pp-browser` (wins over everything below), with `PP_BROWSER_RELAY_ID=relay:…` naming its relay's id (SIGNED_FORMATS § 3.3; its certificate is fetched from it: without it a release build does not use that relay; a developer build pins it on first use). It selects a `custom-<hash>` profile, so the same install launched with and without it has two separate identities.

Precedence: `PP_BROWSER_ORIGIN` > the backend picked in Me → Network (`backend.env` in the config file: `prod` → `PP_API_URL_PROD` (never in a tester build), `staging` (and a saved `qa`) → `PP_API_URL_STAGING`, `dev` → `PP_API_URL_DEV`, `custom` → the relay / directory / registration URLs saved in the config file). Each backend has its own local profile, so switching never mixes identities ([DATA_LAYOUT.md](../contracts/DATA_LAYOUT.md#per-backend-profiles)). `XDG_DATA_HOME` / `--config` still keep whole data sets apart.

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
./scripts/check/check_base_includes.sh
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
ctest --test-dir build -R "TurnPlanTest|ConfigJsonTest|SchemaVersionTest" --output-on-failure
```

## Android (local)

Build a debug APK with the Gradle project under [`android/`](../android/). The native library is built from the repo root [`CMakeLists.txt`](../CMakeLists.txt) via NDK and produces `libmain.so` (SDL convention).

### Prerequisites

- Android SDK (API 35) and NDK r28 (`28.0.13004108`; same pin as CI / `android/app/build.gradle`)
- JDK 17+
- Optional: Android emulator (API 34+, x86_64 image recommended on Linux hosts)

Set environment variables:

```bash
export ANDROID_SDK_ROOT=/path/to/android/sdk
export ANDROID_NDK_HOME=/path/to/android/ndk
```

### Build and install

```bash
./scripts/platform/android_build.sh apk          # assembleDebug, arm64-v8a only (PP_ANDROID_ABIS=arm64-v8a,x86_64 for more)
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

Device builds and code signing: [IOS_BUILD.md](IOS_BUILD.md). TestFlight upload from Actions: [ios-testflight.yml](../../.github/workflows/ios-testflight.yml) (by hand, or for a release candidate from `release.yml`).

The first clean iOS build can take 15–30 minutes (libp2p + RmlUi + BoringSSL). Assets from [`assets/`](../../assets/) are copied into `PP.app/assets/` automatically.

### UI demo app (iOS simulator and Android, developer only)

A separate app with fixed fake data (12 contacts, 10 direct chats, 2 groups, 1 AI chat, two pictures), no backend address and no mesh, for comparing screens with the design. It does not send, receive, call or register. Never for TestFlight, store or release builds. Plan: `DEV/PLAN-ui-demo-data.md`.

- CMake: `-DPP_BROWSER_UI_DEMO=ON -DPP_BROWSER_DEV_TOOLS=ON`, no `PP_API_URL_PROD` / `_STAGING` / `_DEV`. CMake refuses a desktop or iOS-device build, and a build with a backend address ([`cmake/UiDemo.cmake`](../../cmake/UiDemo.cmake)). `scripts/check/check_ui_demo_refused.sh` checks that on the host before you push.
- iOS simulator: bundle id `dev.pp-browser.ios.uidemo`, name "PP Demo"; it lives next to the real app with its own data. The fixed unlock PIN is `123456`.
- Android: `cd android && PP_BROWSER_DEV_TOOLS=1 ./gradlew assembleRelease -PppBrowserUiDemo=true -PppBrowserAbiFilters=arm64-v8a` (application id suffix `.uidemo`; `PP_API_URL_*` are not passed). `assets/demo/` is packed only in this build.
- The pictures are made by `python3 scripts/ui/gen_demo_images.py` into [`assets/demo/`](../../assets/demo/).
