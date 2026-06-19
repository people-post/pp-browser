# Platforms

## Phase 1: Desktop

pp-browser ships as a desktop SDL3 + OpenGL3 application. Path resolution uses XDG on Linux, Application Support on macOS, and AppData on Windows.

## Future: Android / iOS

The following hooks are in place; mobile builds are **not** part of Phase 1.

| Component | Desktop today | Mobile later |
|-----------|---------------|--------------|
| `Platform::Detect()` | Always `Desktop` | SDL platform hint → `Android` / `IOS` |
| `IPathProvider` | `DesktopPathProvider` | Sandbox-internal storage |
| `IAssetLocator` | Source/bundle path via `PP_BROWSER_ASSETS_DIR` | APK/iOS bundle resources |
| `PlatformDefaults` | Ollama localhost | Cloud LLM + API key required |
| `ICredentialStore` | `EnvCredentialStore` | Keychain / Keystore |
| `MessagingHub` | Foreground poll loop | Relay + push; background suspend |
| Navigation | Escape dismiss stack | System back → `ShellHost::HandleDismiss()` |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

## Build notes (future)

- OpenGL ES or Metal renderer backend
- CMake Android NDK / Xcode project wiring
- Disable or defer in-tree libp2p on mobile targets
- MCP via HTTP URL only (no subprocess on iOS)
