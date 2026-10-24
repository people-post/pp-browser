# Push notifications — current state

**Last updated:** 2026-10-12

## Landed (wave 1 + iOS)

| Area | State |
|------|-------|
| Project docs | `projects/push-notifications/` |
| Prefs | `ProfilePreferences.show_notifications` (Me → Profile select) |
| Platform | `ILocalNotifier`, `IPushDeviceRegistrar`, `BackgroundSyncScheduler`, desktop + Android + iOS notifiers |
| Net | `HttpPushDeviceClient` + relay-api device sign ops 3/4 |
| Messaging | `SyncInboxFromWake`, background unread → OS notify when alerts on |
| Relay contract | Client calls `/v1/devices/register|unregister`; expects opaque wakes from the provider when tokens are registered |
| Android | FCM (opt-in via `google-services.json`), WorkManager, NotificationCompat, JNI wake/token |
| iOS | APNs token via `IosPushDeviceRegistrar` + `IosApnsAppDelegate` (SDLUIKitDelegate subclass); silent `inbox_wake` → `BackgroundSyncScheduler`; the relay's `call_wake` alert (P012): tap or foreground → call wake sync; `IosLocalNotifier` posts banners after a wake (tap → open thread); CallKit builds: PushKit token (`ios_voip`) + CallKit screen via `CallSystemUiBridge` (P013) |
| www relay | `FcmPushSender` + `ApnsPushSender` (HTTP/2 + .p8 JWT); branch on `platform` |
| Desktop local banners | Native OS APIs (Linux Freedesktop/D-Bus, macOS `UNUserNotificationCenter`, Windows WinRT toasts); tap → raise + open thread; `ClearForThread` on select |
| Desktop lifecycle | Minimize / focus-lost read as not attentive so banners fire; the app keeps working (calls ring) — not background (P010) |
| Background (phones) | Push only (P010): work paused; a push / WorkManager wake resumes it for 20 s |
| Stay connected (Android, opt-in) | P011: foreground service keeps the work running in the background; relay polled every 30 s |
| Contracts | `SERVICE_ENDPOINTS.md`, `PLATFORMS.md` updated |
| `call_wake` | Opaque type + Android JNI + iOS alert handler + client fetch-then-ring (P008 / V006); a call invite's relay send is marked `urgent` (P012), the relay wakes with `call_wake` |

## Ops (client)

| Item | Purpose |
|------|---------|
| `android/app/google-services.json` | Enables FCM source set in the Android build (provider-agnostic Firebase project) |
| Linux runtime | `libdbus-1` (linked at build via `libdbus-1-dev` / pkg-config) |
| iOS profiles | Development / App Store profiles with Push → `aps-environment` |

## Ops (relay / www)

| Env | Purpose |
|-----|---------|
| `BRF_WWW_FCM_*` | Android FCM HTTP v1 |
| `BRF_WWW_APNS_KEY_ID` / `TEAM_ID` / `KEY_PATH` / `BUNDLE_ID` | iOS APNs token auth |
| `BRF_WWW_APNS_USE_SANDBOX` | USB/dev tokens only (`api.sandbox.push.apple.com`) |

## Follow-ups

- FCM/APNs token refresh → automatic re-register after native start (handler exists; harden cold-start race)
- Android notification tap → open specific thread in ChatController (PendingIntent already embeds `thread_id`)
- Relay (www): honour `urgent` — `call_wake` to Android, the time-sensitive alert to iOS, rate limit (P012)
- iOS CallKit (P013): verify on a device — lock-screen answer (audio via CallKit's session), PIN-locked answer, decline; in-app mute is not mirrored to CallKit's screen yet
