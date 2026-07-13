# Platforms

**Tier:** architecture

## Desktop

pp-browser ships as a desktop SDL3 + OpenGL3 application. Path resolution uses XDG on Linux, Application Support on macOS, and AppData on Windows.

## Android (milestone 2)

Android builds use Gradle + NDK (`android/`) and produce a debug APK with `libmain.so`. The chat shell uses OpenGL ES 3.0 in the RmlUi backend.

| Component | Desktop | Android |
|-----------|---------|---------|
| `Platform::Detect()` | `Desktop` | `Android` |
| `IPathProvider` | `DesktopPathProvider` | `AndroidPathProvider` (internal storage) |
| `IAssetLocator` | `PP_BROWSER_ASSETS_DIR` / bundle | APK assets via `AndroidAssetLocator` + `SdlAssetFileInterface` |
| `AssetIO` | `std::ifstream` on bundle path | `SDL_IOFromFile` on relative APK paths |
| `PlatformDefaults` | Brief LLM + network (`https://www.brief.global`) | Same as desktop |
| `ICredentialStore` | `EnvCredentialStore` | `EnvCredentialStore` (inline API key in Settings; Keystore deferred) |
| libp2p | Built and linked (`p2p::p2p`) | Built and linked (same vendor tree) |
| `MessagingHub` | Foreground poll loop | `BackgroundSyncScheduler`: 2s foreground / ~45s background; FCM wake + WorkManager when enabled |
| Navigation | Escape → dismiss then exit | Back → dismiss then minimize; Escape same as desktop |
| MCP stdio | Supported | Skipped; use `mcp.url` |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

### Android GL lifecycle (rotation / Recents)

Android keeps the activity alive across orientation changes (`configChanges` in the manifest). SDL tears down and restores the EGL surface on pause/resume and during some rotations.

| Event | App response |
|-------|----------------|
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` / display scale / safe area | `Backend::SyncContext` updates RmlUi dimensions, DP ratio, and GL viewport; next frame is forced (no long power-save wait) |
| `SDL_EVENT_RENDER_DEVICE_RESET` | Rebuild GL3 shaders/FBOs, release TextLoupe GPU state, `Rml::ReleaseTextures` / `ReleaseCompiledGeometry` / `ReleaseFontResources`, then `SyncContext` |
| `SDL_EVENT_DID_ENTER_FOREGROUND` | `SyncContext` + theme sync (size may have changed while backgrounded) |
| Invalid surface / zero pixel size | Skip `BeginFrame` / `PresentFrame` (avoids clearing into a dead surface) |
| `onPause` / focus loss (before surface destroy) | `MainActivity` best-effort `PixelCopy` of `SDLSurface` into `TaskDescription` so Recents is less likely to show a black tile |

Do not issue OpenGL calls after `SDL_EVENT_WILL_ENTER_BACKGROUND`; SDL may have already backed up the EGL context.

Build: [BUILD.md](../ops/BUILD.md#android-local).

## iOS (reserved)

iOS is not shipped yet. Shared abstractions exist for a future Xcode target:

| Component | Status |
|-----------|--------|
| `PlatformKind::IOS` | Defined |
| `IosPathProvider` / `IosAssetLocator` | Stubs use relative bundle paths (same as Android) |
| `SdlAssetFileInterface` | Ready for iOS bundle reads |
| CMake | `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64` |
| Renderer | GLES via SDL (same pattern as Android); Metal is a later fork change |
| libp2p | Built and linked on all platforms |
| MCP | HTTP URL only (no subprocess on iOS) |

## Deferred

- Android Keystore / iOS Keychain for API keys
- Release signing and Play distribution

## libp2p background

On `AppLifecycle::OnWillEnterBackground`, messaging suspends cold peer connections (`PeerSessionManager::SuspendColdPeers`) while keeping the warm (active-thread) set. Foreground resume may re-warm the open thread.

Inbox sync uses [`BackgroundSyncScheduler`](../../src/base/platform/BackgroundSyncScheduler.h): foreground poll every 2s; while backgrounded and the process is alive, poll every ~45s (IO temporarily held open). Android additionally uses WorkManager (~15 min when notifications are off; rarer backup when on) and optional FCM opaque wakes when `google-services.json` is present. See [projects/push-notifications](../../projects/push-notifications/).
