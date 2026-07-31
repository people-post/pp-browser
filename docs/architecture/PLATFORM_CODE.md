# Platform code layout

**Tier:** architecture

Where OS-specific code lives and how to add new platform behavior without scattering `#ifdef`s through feature or data layers.

See also: [PLATFORMS.md](PLATFORMS.md) (runtime matrix), [SRC_LAYOUT.md](SRC_LAYOUT.md) (layer rules).

## Policy

1. **OS `#ifdef`s belong in dedicated files** under [`src/base/platform/os/`](../../src/base/platform/os/) (primitives) or [`src/base/platform/desktop/`](../../src/base/platform/desktop/) (desktop Win/macOS/Linux). Mobile uses top-level `Android*` / `Ios*` types.
2. **Other layers call portable APIs** — `Platform::`, `IPathProvider`, `OsFile`, `OsTime`, etc. No Win32 or POSIX headers outside the platform layer (except render GL integration; see below).
3. **Runtime dispatch preferred over `#ifdef` in business logic** — use `Platform::Detect()` / `Platform::IsMobile()` when behavior differs by mobile vs desktop, not by OS family.
4. **Render GL / GLES** stays in [`src/render/integration/platform/`](../../src/render/integration/platform/) per SRC_LAYOUT; do not move GL lifecycle into `base/platform`.

## Directory layout

| Path | Role |
|------|------|
| `base/platform/Platform.{h,cpp}` | `PlatformKind` detection, capability flags |
| `base/platform/PlatformServices.*` | Registers Android/iOS/desktop implementations at startup |
| `base/platform/os/` | Low-level OS primitives (`OsFile`, `OsTime`, `OsProcess`, `OsTlsCaPath` / `OsTlsPlatformCurl`, executable path) |
| `base/platform/desktop/` | Per-OS desktop path and **native** notification implementations (Linux Freedesktop Notifications via linked `libdbus-1`, macOS `UNUserNotificationCenter`, Windows WinRT toasts) — not shell helpers |
| `base/platform/Android*`, `Ios*`, `Desktop*` | Facades implementing `IPathProvider`, `ILocalNotifier`, etc. |
| `base/platform/MobileWindowSizing.*` | SDL display-mode sizing for mobile windows |
| `base/platform/PlatformLogDefaults.*` | Startup log level defaults per platform |
| `base/platform/PlatformStartupHints.*` | User-facing init failure hints (legacy English string_view) |
| `base/platform/PlatformUserHints.*` | Catalog keys for OS tips (Local Network, firewall, mic); UI resolves with `Tr()` |
| `render/integration/platform/GlBackend.h` | GLES vs desktop GL selection |
| `render/integration/platform/MobileGlLifecycle.*` | iOS/Android GL surface and drawable handling |

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

Each `_*_{Win32,Darwin,Linux,Posix}.cpp` file wraps its entire content in a matching `#if defined(...)` guard so all platform TUs can compile in one library until CMake source selection is enabled.

## Allowlisted `#ifdef` locations

Regressions are caught by [`scripts/check_platform_ifdefs.sh`](../../scripts/check_platform_ifdefs.sh).

Allowed paths for OS preprocessor branches:

- `src/base/platform/` (including `os/` and `desktop/`)
- `src/render/integration/` (GL/GLES backends)
- `src/render/fork/` (upstream; not product policy)
- `src/libp2p/fork/` (upstream; not product policy)

**Not allowed** (after consolidation): `src/feature/`, `src/base/data/`, `src/base/net/`, `src/base/ai/`, `src/app/Application.cpp`.

## Adding new platform behavior

1. If it is a path, notification, credential, or lifecycle concern → extend or add an `I*` interface implementation under `base/platform/`.
2. If it is a syscall (file sync, subprocess, time) → add to `base/platform/os/`.
3. If it is GL/GLES or SDL window backend → `render/integration/platform/`.
4. Wire registration in `PlatformServices::Register()` for mobile overrides; desktop defaults stay in interface singletons.
