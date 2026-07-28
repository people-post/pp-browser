# Platforms

**Tier:** architecture

Platform code file layout and `#ifdef` policy: [PLATFORM_CODE.md](PLATFORM_CODE.md).

## Desktop

pp-browser ships as a desktop SDL3 + OpenGL3 application. Path resolution uses XDG on Linux, Application Support on macOS, and AppData on Windows.

### Live window resize

On several OSes, `SDL_PollEvent` / `SDL_WaitEventTimeout` block for the whole interactive resize (or move) drag. The main loop cannot Present, so the compositor stretches the last frame — edge-repeat when growing.

Fix: `SDL_AddEventWatch` in `BrowserHost` invokes an app-registered redraw on `PIXEL_SIZE_CHANGED` / `RESIZED` / live `EXPOSED` (`data1 == 1`). The handler (`Application::Run`) runs `SyncContext` → `ShellHost::Update` → `Context::Update` → `BeginFrame` / `Render` / `PresentFrame`. Duplicate size events for the same pixel size are coalesced.

See [SDL wiki: AppFreezeDuringDrag](https://wiki.libsdl.org/SDL3/AppFreezeDuringDrag).

## A/V media (SDL + calls)

Voice/video capture and playback go through **SDL3 audio** (and later camera) in [`CallMediaEngine`](../../src/base/media/CallMediaEngine.cpp) — [p2p-av-calls](../../projects/p2p-av-calls/) V014 / V017 / V018. Signaling is mesh/E2E; media is WebRTC-shaped (libdatachannel). Video encode/decode uses **platform HW H264** (Media Foundation / VideoToolbox / MediaCodec; Linux VA-API best-effort) — not OpenH264 as product default.

| Platform | SDL audio backend | Extra build packages? | Product checklist (not all done) |
|----------|-------------------|----------------------|----------------------------------|
| Linux | PulseAudio + ALSA | **Yes** — `libpulse-dev` + `libasound2-dev` ([BUILD.md](../ops/BUILD.md)) | Dev packages + reconfigure if stuck on dummy |
| Windows | WASAPI | No | OS microphone privacy |
| macOS | CoreAudio | No | Mic privacy / usage string for notarized apps |
| Android | AAudio / OpenSL ES | No | Manifest `RECORD_AUDIO` + runtime permission; optional a3 dogfood: `CAMERA` |
| iOS | CoreAudio | No | **Separate mobile-bring-up** (not a3 exit, V016): `NSMicrophoneUsageDescription`; `AVAudioSession` VoIP/play-and-record; optional background `audio`; later camera usage |

**Agent traps**

| Wrong | Right |
|-------|--------|
| Require Pulse/ALSA on Windows/macOS/mobile | Only Linux needs those `-dev` packages |
| Claim mobile voice without manifest/plist permissions | Add Android/iOS mic (and later camera) entitlements first |
| Assume LAN ICE proves mobile | Mobile NAT needs mesh seed SFU ([p2p-av-calls](../../projects/p2p-av-calls/)) |

`Backend::Initialize` must **not** fail window bring-up on audio — init audio on demand in `CallMediaEngine` (`SDL_InitSubSystem(SDL_INIT_AUDIO)`).

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
| HTTPS (curl + BoringSSL) | Host CA bundle / Secure Transport / Schannel | `os::ApplyPlatformCurlSsl` → system CAPATH (`TlsCaPath`); same entry as iOS via `ApplyCurlSslDefaults` |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

### Android GL lifecycle (rotation / Recents)

Android keeps the activity alive across orientation changes (`configChanges` in the manifest). SDL tears down and restores the EGL surface on pause/resume and during some rotations.

| Event | App response |
|-------|----------------|
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` / display scale / safe area | `Backend::SyncContext` updates RmlUi dimensions, DP ratio, and GL viewport; next frame is forced (no long power-save wait) |
| UI task posted (`BrowserThread::PostTask(UI)`) | `Backend::WakeEventLoop` pushes a coalesced SDL user event so `SDL_WaitEventTimeout` returns promptly |
| Shell timers (gesture dismiss slide-out, toast expiry) | `ShellHost::NotifyFrameEnd` calls `Context::RequestNextUpdate` **after** `Context::Update` so power-save wakes for the next deadline without waiting for input |
| Idle power-save wait | Capped at **2s** (foreground relay poll cadence). A 10s cap starved `BackgroundSyncScheduler` / badge refresh on touch-idle Android until the user tapped |
| `SDL_EVENT_RENDER_DEVICE_RESET` | Rebuild GL3 shaders/FBOs, release TextLoupe GPU state, `Rml::ReleaseTextures` / `ReleaseCompiledGeometry` / `ReleaseFontResources`, then `SyncContext` |
| `SDL_EVENT_DID_ENTER_FOREGROUND` | `SyncContext` + theme sync (size may have changed while backgrounded) |
| Invalid surface / zero pixel size | Skip `BeginFrame` / `PresentFrame` (avoids clearing into a dead surface) |
| `onPause` / focus loss (before surface destroy) | `MainActivity` best-effort `PixelCopy` of `SDLSurface` into `TaskDescription` so Recents is less likely to show a black tile |

Do not issue OpenGL calls after `SDL_EVENT_WILL_ENTER_BACKGROUND`; SDL may have already backed up the EGL context.

**A/V:** `RECORD_AUDIO` for voice; optional a3 dogfood also needs `CAMERA` + runtime prompt before `CallMediaEngine` opens capture. See [§ A/V media](#av-media-sdl--calls).

Build: [BUILD.md](../ops/BUILD.md#android-local).

## iOS (scaffold)

iOS builds use CMake + Xcode toolchains from the repo root (no separate Gradle-style wrapper yet). Simulator builds work unsigned; device builds need Apple provisioning.

| Component | Desktop | iOS |
|-----------|---------|-----|
| `Platform::Detect()` | `Desktop` | `IOS` (`TARGET_OS_IPHONE`) |
| `IPathProvider` | `DesktopPathProvider` | `IosPathProvider` (SDL pref path sandbox) |
| `IAssetLocator` | `DesktopAssetLocator` | `IosAssetLocator` → `Frame.app/assets/` |
| `AssetIO` | filesystem | `SdlAssetFileInterface` + bundle-relative paths |
| Renderer | OpenGL 3.3 | OpenGL ES 3 (SDL) |
| libp2p | Built and linked | Built and linked |
| MCP | stdio + HTTP | HTTP URL only |
| Push | N/A (desktop local notify) | APNs deferred — placeholders in `packaging/ios/` |
| HTTPS (curl + BoringSSL) | Secure Transport (macOS) | `os::ApplyPlatformCurlSsl` → Security.framework `SecTrust` verify callback (system roots / MDM); no bundled CA file |

### Mobile HTTPS trust (curl + BoringSSL)

Mobile builds link curl against vendored BoringSSL (no Secure Transport on iOS). BoringSSL has no built-in CA store, so peer verification must use the OS:

| Platform | Mechanism | Tracks OS distrust / MDM roots? |
|----------|-----------|----------------------------------|
| Android | `CURLOPT_CAPATH` → Conscrypt/system hashed PEMs (`os::TlsCaPath`) | Yes (OS CA updates) |
| iOS | `CURLOPT_SSL_CTX_FUNCTION` → `SecTrustEvaluateWithError` | Yes (Keychain trust store) |

Entry point: `ApplyCurlSslDefaults` → `os::ApplyPlatformCurlSsl` (`src/base/platform/os/OsTlsPlatformCurl_*`).

**A/V (mobile-bring-up task, not a3 exit — [V016](../../projects/p2p-av-calls/DECISIONS.md#v016--a3-delivery-slice-lan-video-first-sfu--ios-separate)):** add `NSMicrophoneUsageDescription` to [`packaging/ios/Info.plist`](../../packaging/ios/Info.plist); configure `AVAudioSession` for play-and-record / VoIP before capture; consider `UIBackgroundModes` → `audio` for in-call background. Camera usage strings when iOS video is dogfooded. See [§ A/V media](#av-media-sdl--calls).

Build and signing placeholders: [IOS_BUILD.md](../ops/IOS_BUILD.md). Scripts: [`scripts/ios_build.sh`](../../scripts/ios_build.sh), [`scripts/ios_sign.sh`](../../scripts/ios_sign.sh).

## Deferred

- Android Keystore / iOS Keychain for API keys
- Release signing and Play distribution

## libp2p background

Desktop may run as a mesh **Node** (listen preferred on TCP **18517**, busy-port fallback per [p2p-mesh N016](../../projects/p2p-mesh/DECISIONS.md)) or **Client** (`node_enabled` off). Mobile is always Client — outbound dials only, no listen UI. See [p2p-mesh](../../projects/p2p-mesh/) and [CONFIGURATION.md](../ops/CONFIGURATION.md).

On `AppLifecycle::OnWillEnterBackground`, messaging suspends cold peer connections (`PeerSessionManager::SuspendColdPeers`) while keeping the warm (active-thread) set. Foreground resume may re-warm the open thread.

Inbox sync uses [`BackgroundSyncScheduler`](../../src/base/platform/BackgroundSyncScheduler.h): foreground poll every 2s; while backgrounded and the process is alive, poll every ~45s (IO temporarily held open). Android additionally uses WorkManager (~15 min when notifications are off; rarer backup when on) and optional FCM opaque wakes when `google-services.json` is present. See [projects/push-notifications](../../projects/push-notifications/).
