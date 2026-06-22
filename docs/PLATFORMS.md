# Platforms

## Desktop

pp-browser ships as a desktop SDL3 + OpenGL3 application. Path resolution uses XDG on Linux, Application Support on macOS, and AppData on Windows.

## Android (milestone 1)

Android builds use Gradle + NDK (`android/`) and produce a debug APK with `libmain.so`. The chat shell UI uses the existing OpenGL ES 3.2 path in the RmlUi backend.

| Component | Desktop | Android |
|-----------|---------|---------|
| `Platform::Detect()` | `Desktop` | `Android` |
| `IPathProvider` | `DesktopPathProvider` | `AndroidPathProvider` (internal storage) |
| `IAssetLocator` | `PP_BROWSER_ASSETS_DIR` / bundle | APK assets via `AndroidAssetLocator` + `AndroidFileInterface` |
| `PlatformDefaults` | Ollama localhost | Same as desktop for now (cloud defaults deferred) |
| `ICredentialStore` | `EnvCredentialStore` | `EnvCredentialStore` (Keystore deferred) |
| libp2p | Built and linked (`p2p::p2p`) | Built and linked (same vendor tree) |
| `MessagingHub` | Foreground poll loop | Same; background suspend deferred |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

Build: [BUILD.md](BUILD.md#android-local).

## iOS (reserved)

iOS is not shipped yet. Shared abstractions exist for a future Xcode target:

| Component | Status |
|-----------|--------|
| `PlatformKind::IOS` | Defined |
| `IosPathProvider` / `IosAssetLocator` | Stubs compiled; wired when iOS ships |
| CMake | `-DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64` |
| Renderer | GLES via SDL (same pattern as Android); Metal is a later fork change |
| libp2p | Built and linked on all platforms (same as desktop/Android) |
| MCP | HTTP URL only (no subprocess on iOS) |

## Deferred (mobile milestone 2)

- System back → `ShellHost::HandleDismiss()`
- Cloud LLM + API key defaults; Android Keystore / iOS Keychain
- libp2p background suspend and relay fallback
- MCP stdio spawn guard on mobile
