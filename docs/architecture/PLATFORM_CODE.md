# Platform code layout

**Tier:** architecture

Where OS-specific code lives and how to add new platform behavior without scattering `#ifdef`s through feature or data layers.

See also: [PLATFORMS.md](PLATFORMS.md) (runtime matrix), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layer rules).

## Policy

OS-specific code has **three homes**, not one folder:

| Home | What goes there | Test |
|------|-----------------|------|
| **`foundation/platform/`** | Cross-cutting OS services many modules need | Paths, files, process, TLS trust, notifications, assets, connectivity, push JNI |
| **Domain `*_Win32.cpp` / `*_Android.cpp` next to the feature** | Fat OS backends used by one module | H264, camera orientation, VoIP audio session, mDNS sockets, GL lifecycle, net-if enumeration |
| **Runtime dispatch (`Platform::Detect()` / `IsMobile()`)** | Product behavior, not syscalls | Portrait lock, poll intervals, skip stdio MCP |

Do **not** move media codecs, camera, or call audio-session backends into `foundation/platform/` — that would reverse the module graph (`pp_base_media` already depends on `pp_base_platform`) and mix shared OS services with domain types.

Hard rules:

1. **OS `#ifdef`s belong in dedicated backend files** (naming below) or in `foundation/platform/` / `base/render/`. Portable TUs (`CallMediaEngine.cpp`, `LanMdnsDiscovery.cpp`, `Reachability.cpp`, feature/app logic) call portable APIs only.
2. **Other layers call portable APIs** — `Platform::`, `IPathProvider`, `OsFile`, `IVideoCodec`, `CallAudioSession`, etc. No Win32, POSIX, or JNI headers outside backend files and `foundation/platform/` (except render GL integration).
3. **Runtime dispatch preferred over `#ifdef` in business logic** — use `Platform::Detect()` / `Platform::IsMobile()` when behavior differs by mobile vs desktop, not by OS family.
4. **Process runtime** (threads, lifecycle, branding) lives in [`src/foundation/runtime/`](../../src/foundation/runtime/) — not here.
5. **Public headers must not expose OS types** (`SOCKET`, `HANDLE`, `NativeSocket`). Hide them in the `.cpp` or a private impl header.
6. **`common/` CRT shims** — thread naming (`WorkerPool`) and civil-time (`CivilTime`) may use CRT/pthread `#ifdef`s. New OS behavior does not belong in `common/` beyond that.

## Directory layout

| Path | Role |
|------|------|
| `foundation/runtime/` | `AppRuntime`, coordinator, `AppLifecycle`, `BackgroundSyncScheduler`, product branding/version |
| `foundation/platform/Platform.{h,cpp}` | `PlatformKind` detection, capability flags |
| `foundation/platform/PlatformServices.*` | Registers Android/iOS/desktop implementations at startup |
| `foundation/platform/SdlAppEvents.*`, `AppEventHooks.*` | SDL lifecycle / input pre-process → `AppLifecycle` |
| `foundation/platform/os/` | Low-level OS primitives (`OsFile`, `OsProcess`, `OsTlsCaPath` / `OsTlsPlatformCurl`, executable path). Civil time is implemented in `common/CivilTime` and re-exported as `pbr::os::{TimeGm,LocalTime,UtcTime}`. |
| `foundation/platform/desktop/` | Per-OS desktop path and **native** notification implementations (Linux Freedesktop Notifications via linked `libdbus-1`, macOS `UNUserNotificationCenter`, Windows WinRT toasts) — not shell helpers |
| `foundation/platform/Android*`, `Ios*`, `Desktop*` | Facades implementing `IPathProvider`, `ILocalNotifier`, etc. |
| `foundation/platform/MobileWindowSizing.*` | SDL display-mode sizing for mobile windows |
| `foundation/platform/PlatformLogDefaults.*` | Startup root log level + emit floor defaults per platform |
| `foundation/platform/PlatformStartupHints.*` | User-facing init failure hints (legacy English string_view) |
| `foundation/platform/PlatformUserHints.*` | Catalog keys for OS tips (Local Network, firewall, mic); UI resolves with `Tr()` |
| `base/media/VideoCodec_*.cpp` | Platform HW H264 (`IVideoCodec` / `CreateOsVideoCodec`) |
| `base/media/CallAudioSession_*.{cpp,mm}` | VoIP audio session + capture-open policy |
| `base/media/CameraCaptureOrientation_*.{cpp,mm}` | Upright camera transform |
| `base/mesh/LanMdnsSocket_*.cpp` | UDP multicast for LAN mDNS |
| `base/mesh/ReachabilityNetIf_*.cpp` | Interface address enumeration |
| `pp-cpp-ui backend/GlBackend.h` | GLES vs desktop GL selection |
| `base/render/platform/MobileGlLifecycle.*` | iOS/Android GL surface and drawable handling |

## User-facing OS tips (i18n)

Do **not** embed English Settings paths or `#ifdef` string catalogs in `feature/` or `app/`.

```text
1. Add catalog key under hints.* (en + zh-Hans); use {product} when naming the app
2. Map OS → key only in PlatformUserHints.cpp (allowlisted #ifdef)
3. UI: Tr(PlatformUserHints::…Key(), {{"product", kProductName}})
4. Domain code passes facts (bools/enums), never finished copy
```

New product tips use keys. `InitFailureHint()` remains a rare English string_view for early startup before catalogs load; migrate later if needed.

## File naming

| Pattern | Example |
|---------|---------|
| OS primitive | `os/OsFile.h`, `os/OsFile_Win32.cpp`, `os/OsFile_Posix.cpp` |
| Desktop per-OS | `desktop/PathProvider_Win32.cpp`, `desktop/LocalNotifier_Linux.cpp` |
| Facade | `DesktopPathProvider.cpp` delegates to `desktop::*Impl()` |
| Domain backend | `VideoCodec_Win32.cpp`, `LanMdnsSocket_Posix.cpp`, `CallAudioSession_Android.cpp` |

CMake **source-selects** the matching backend (do not compile MediaCodec TUs on Linux). File-level `#if defined(...)` guards may remain as belt-and-suspenders.

## Allowlisted `#ifdef` locations

Regressions are caught by [`scripts/check_platform_ifdefs.sh`](../../scripts/check_platform_ifdefs.sh) (CI lint job).

Allowed paths for OS preprocessor branches:

- `src/foundation/platform/` (including `os/` and `desktop/`)
- `src/base/render/` (GL/GLES backends)
- Dedicated backend files: `*_Win32`, `*_Posix`, `*_Darwin`, `*_Linux`, `*_Android`, `*_Ios`, `*_Default` (`.cpp` / `.mm` / `.h`) under `src/base/media/` and `src/base/mesh/`
- `src/common/CivilTime.cpp`, `src/common/WorkerPool.cpp`, `src/common/Logger.h` (CRT / pthread / Windows.h macro shims only)
- `**/tests/**` (test harness env/path helpers)
- `pp-cpp-ui rmlui/` (upstream; not product policy)
- `src/lib/libp2p/` (upstream; not product policy)

**Not allowed:** `src/feature/`, `src/app/` (except tests), `src/foundation/data/`, `src/base/net/`, `src/base/ai/`, portable TUs such as `CallMediaEngine.cpp`, `LanMdnsDiscovery.cpp`, `Reachability.cpp`.

## Adding new platform behavior

1. If it is a path, notification, credential, or OS tip → extend or add an `I*` interface implementation under `foundation/platform/`.
2. If it is process threading / lifecycle / scheduling → `foundation/runtime/` (`AppRuntime`, `AppLifecycle`).
3. If it is a syscall used by many modules (file sync, subprocess) → add to `foundation/platform/os/`. Civil time → `common/CivilTime` (re-exported as `pbr::os::`).
4. If it is GL/GLES or SDL window backend → pp-cpp-ui `backend/` (shared) or `base/render/` (product-only).
5. If it is a fat backend used by one module (codec, camera, audio session, raw sockets, net-if) → colocated `*_Win32.cpp` / `*_Android.cpp` / … behind a portable header in that module. Wire with CMake source-selection.
6. Wire registration in `PlatformServices::Register()` for mobile overrides of `I*` facades; desktop defaults stay in interface singletons.
