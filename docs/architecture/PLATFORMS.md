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

Voice/video capture and playback go through **SDL3 audio/camera** in [`CallMediaEngine`](../../src/domain/media/CallMediaEngine.cpp) — [p2p-av-calls](../../projects/p2p-av-calls/) V014 / V017–V019 / **V026**. Signaling is mesh/E2E; **media is libp2p** (direct `/pp-browser/realtime/1.0.0` or blind `media_relay`). Every active call encodes Opus + H264 (V019); video encode/decode uses **platform HW H264** (Media Foundation / VideoToolbox / MediaCodec; Linux **libva DRM** VA-API best-effort) — not OpenH264 as product default. Audio is mandatory; video is best-effort. Mobile upright capture uses [`CameraCaptureOrientation`](../../src/domain/media/CameraCaptureOrientation.h) (Android Camera2 `SENSOR_ORIENTATION` + display rotation; iOS interface orientation).

| Platform | SDL audio backend | Extra build packages? | Product checklist (not all done) |
|----------|-------------------|----------------------|----------------------------------|
| Linux | PulseAudio + ALSA | **Yes** — `libpulse-dev` + `libasound2-dev`; video: `libva-dev` ([BUILD.md](../ops/BUILD.md)) | Dev packages + reconfigure if stuck on dummy; VA driver at runtime for H264; LAN video **receive** from Android/Win/Mac OK 2026-07-31 (camera-less dogfood host) |
| Windows | WASAPI | No | OS microphone privacy; LAN 1:1 dogfood vs Android/Linux/**macOS** (Win↔Mac bidirectional OK 2026-07-31) |
| macOS | CoreAudio | No | Mic / camera + **macOS 15+ Local Network** (`NSLocalNetworkUsageDescription` in [`packaging/macos/Info.plist`](../../packaging/macos/Info.plist)); LAN 1:1 dogfood vs Android/Linux/**Windows** (Win↔Mac bidirectional OK 2026-07-31) |
| Android | AAudio / OpenSL ES | No | Manifest `RECORD_AUDIO` + `CAMERA` + runtime; link `mediandk` + `camera2ndk`; LAN bidirectional vs Win/Mac OK 2026-07-31 |
| iOS | CoreAudio | No | a3 wiring (V016): `NSMicrophoneUsageDescription` + `NSCameraUsageDescription`; `AVAudioSession` VoIP/play-and-record; `UIBackgroundModes` `audio`; device dogfood optional |

**Agent traps**

| Wrong | Right |
|-------|--------|
| Require Pulse/ALSA on Windows/macOS/mobile | Only Linux needs those `-dev` packages |
| Claim mobile voice without manifest/plist permissions | Add Android/iOS mic (and later camera) entitlements first |
| Assume LAN ICE proves mobile NAT | Mobile NAT needs mesh seed SFU ([p2p-av-calls](../../projects/p2p-av-calls/)) |
| Ship macOS PP.app without Local Network usage string | Add `NSLocalNetworkUsageDescription` (and Bonjour services key); otherwise Sequoia silently blocks ICE host UDP to Android/LAN peers |
| Dogfood LAN ICE only from Cursor’s integrated terminal | Prefer a normal OS terminal or packaged `.app` — Cursor’s env can break host UDP while signaling still works |
| Hardcode Android camera rotation | Use `CameraCaptureOrientation` / Camera2 metadata |

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
| `ConversationsHub` | Foreground poll loop | `BackgroundSyncScheduler`: 2s foreground / ~45s background; FCM wake + WorkManager when enabled |
| Navigation | Escape → dismiss then exit | Back → dismiss then minimize; Escape same as desktop |
| MCP stdio | Supported | Skipped; use `mcp.url` |
| HTTPS (curl + BoringSSL) | Host CA bundle / Secure Transport / Schannel | `os::ApplyPlatformCurlSsl` → system CAPATH (`TlsCaPath`); same entry as iOS via `ApplyCurlSslDefaults` |

Profile-scoped data layout (`profiles/{id}/`) is unchanged on mobile.

### Android GL lifecycle (rotation / Recents)

**UI orientation is locked to portrait** (`android:screenOrientation="portrait"` + `SDL_HINT_ORIENTATIONS=Portrait` before `SDL_Init`). The shell does not follow device rotation — avoids EGL surface churn, cropped/black frames, and in-call video crashes. Free rotation is deferred polish.

Android still keeps the activity alive across other config changes (`configChanges` in the manifest). SDL tears down and restores the EGL surface on pause/resume (and would during rotations if unlocked).

| Event | App response |
|-------|----------------|
| Soft keyboard (IME) | Manifest `windowSoftInputMode=adjustNothing` (do not resize/pan EGL). `MainActivity` publishes **keyboard-only** bottom inset via IMM/`WindowInsets` (never status/nav — those are already window-fitted on API &lt; 30) → SDL safe-area → `ShellHost::RefreshSafeAreaInsets`. Same channel as iOS; moto g(7) play / API 28 verified. In-app emoji **Insert** on mobile/compact uses `ShellHost::SetBottomChrome` (latched IME height, remount-only, no scrim) |
| `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` / display scale / safe area | `Backend::SyncContext` updates RmlUi dimensions, DP ratio, and GL viewport; next frame is forced (no long power-save wait) |
| UI task posted (`AppRuntime::PostUI`) | `SetUIWakeCallback` → `Backend::RequestForceFrame` (force next poll + always-push wake event). `Application::Run` also forces when `HasPendingUITasks()`. Do **not** use `WakeEventLoop` alone — that left SyncLayout/call chrome pending until mouse move when WaitEventTimeout lied |
| Call chrome dirty (`Backend::RequestForceFrame`) | Skips the next idle wait so ring/accept overlays Present without waiting for input (also implied by PostTask(UI)) |
| `ShellHost::RequestSyncLayout` | Posts flush + `RequestForceFrame` so deferred remounts do not stall behind idle wait |
| Shell timers (gesture dismiss slide-out, toast expiry) | `ShellHost::NotifyFrameEnd` calls `Context::RequestNextUpdate` **after** `Context::Update` so power-save wakes for the next deadline without waiting for input |
| Idle power-save wait | **Poll + ≤50ms Delay slices** capped at **2s**. Mid-idle abort if `force_next_frame` / wake pending. **Forbidden:** `SDL_WaitEventTimeout` for idle — some X11/SDL builds ignored the timeout until real input (Sessions tab), freezing Present while coordinator poll still advanced |
| `SDL_EVENT_RENDER_DEVICE_RESET` | Rebuild GL3 shaders/FBOs, release TextLoupe GPU state, `Rml::ReleaseTextures` / `ReleaseCompiledGeometry` / `ReleaseFontResources`, then `SyncContext` |
| `SDL_EVENT_DID_ENTER_FOREGROUND` | `SyncContext` + theme sync (size may have changed while backgrounded) |
| Invalid surface / zero pixel size | Skip `BeginFrame` / `PresentFrame` (avoids clearing into a dead surface) |
| `onPause` / focus loss (before surface destroy) | `MainActivity` best-effort `PixelCopy` of `SDLSurface` into `TaskDescription` so Recents is less likely to show a black tile |

Do not issue OpenGL calls after `SDL_EVENT_WILL_ENTER_BACKGROUND`; SDL may have already backed up the EGL context.

**A/V:** `RECORD_AUDIO` for voice; a3 also needs `CAMERA` + runtime prompt before `CallMediaEngine` opens capture (V019). See [§ A/V media](#av-media-sdl--calls).

Build: [BUILD.md](../ops/BUILD.md#android-local).

## iOS (scaffold)

iOS builds use CMake + Xcode toolchains from the repo root (no separate Gradle-style wrapper yet). Simulator builds work unsigned; device builds need Apple provisioning.

| Component | Desktop | iOS |
|-----------|---------|-----|
| `Platform::Detect()` | `Desktop` | `IOS` (`TARGET_OS_IPHONE`) |
| `IPathProvider` | `DesktopPathProvider` | `IosPathProvider` (SDL pref path sandbox) |
| `IAssetLocator` | `DesktopAssetLocator` | `IosAssetLocator` → `PP.app/assets/` |
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

Entry point: `ApplyCurlSslDefaults` → `os::ApplyPlatformCurlSsl` (`src/foundation/platform/os/OsTlsPlatformCurl_*`).

**A/V (a3 wiring — [V016](../../projects/p2p-av-calls/DECISIONS.md#v016--a3-delivery-slice-lan-video-mobile-wiring-included)):** `NSMicrophoneUsageDescription` + `NSCameraUsageDescription` in [`packaging/ios/Info.plist`](../../packaging/ios/Info.plist); `AVAudioSession` play-and-record / VoIP via `CallAudioSession` before capture; `UIBackgroundModes` → `audio` for in-call background. **Portrait-only** (`UISupportedInterfaceOrientations` + `SDL_HINT_ORIENTATIONS`) — same rotation lock as Android. Physical device dogfood optional.

Build and signing placeholders: [IOS_BUILD.md](../ops/IOS_BUILD.md). Scripts: [`scripts/platform/ios_build.sh`](../../scripts/platform/ios_build.sh), [`scripts/platform/ios_sign.sh`](../../scripts/platform/ios_sign.sh).

## Deferred

- Android Keystore / iOS Keychain for API keys
- Release signing and Play distribution

## libp2p background

Desktop may run as a mesh **Node** (listen preferred on TCP **18517**, busy-port fallback per [p2p-mesh N016](../../projects/p2p-mesh/DECISIONS.md)) or **Client** (`node_enabled` off). Mobile defaults to **Client** (no always-on listen). During a **foreground call on Wi‑Fi**, ephemeral listen may run per [N025](../../projects/p2p-mesh/DECISIONS.md#n025--mobile-call-scoped-listen-on-wi-fi-not-full-node) / [V027](../../projects/p2p-av-calls/DECISIONS.md#v027--mobile-call-scoped-listen-on-wi-fi) — not full Node. See [p2p-mesh](../../projects/p2p-mesh/) and [CONFIGURATION.md](../ops/CONFIGURATION.md).

On `AppLifecycle::OnWillEnterBackground`, messaging suspends cold peer connections (`PeerSessionManager::SuspendColdPeers`) while keeping the warm (active-thread) set. Foreground resume may re-warm the open thread.

Inbox sync uses [`BackgroundSyncScheduler`](../../src/foundation/runtime/BackgroundSyncScheduler.h): foreground poll every 2s; while backgrounded and the process is alive, poll every ~45s (IO temporarily held open). Android additionally uses WorkManager (~15 min when notifications are off; rarer backup when on) and optional FCM opaque wakes when `google-services.json` is present. See [projects/push-notifications](../../projects/push-notifications/).
