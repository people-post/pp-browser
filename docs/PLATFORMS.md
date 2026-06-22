# Platforms

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
| `PlatformDefaults` | Cloud LLM (`https://api.openai.com/v1`) | Same as desktop |
| `ICredentialStore` | `EnvCredentialStore` | `EnvCredentialStore` (inline API key in Settings; Keystore deferred) |
| libp2p | Built and linked (`p2p::p2p`) | Built and linked (same vendor tree) |
| `MessagingHub` | Foreground poll loop | Poll paused in background via `AppLifecycle` |
| Navigation | Escape → dismiss then exit | Back → dismiss then minimize; Escape same as desktop |
| MCP stdio | Supported | Skipped; use `mcp.url` |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

Build: [BUILD.md](BUILD.md#android-local).

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
- libp2p background suspend and relay fallback
- Release signing and Play distribution
