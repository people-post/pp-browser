# Push notifications — current state

**Last updated:** 2026-07-28

## Landed (wave 1)

| Area | State |
|------|-------|
| Project docs | `projects/push-notifications/` |
| Prefs | `ProfilePreferences.show_notifications` (Me → Profile select) |
| Platform | `ILocalNotifier`, `IPushDeviceRegistrar`, `BackgroundSyncScheduler`, desktop + Android notifiers |
| Net | `HttpPushDeviceClient` + relay-api device sign ops 3/4 |
| Messaging | `SyncInboxFromWake`, background unread → OS notify when alerts on |
| Relay contract | Client calls `/v1/devices/register|unregister`; expects opaque FCM wakes from the provider when tokens are registered |
| Android | FCM (opt-in via `google-services.json`), WorkManager, NotificationCompat, JNI wake/token |
| Desktop local banners | Native OS APIs (Linux Freedesktop/D-Bus, macOS `UNUserNotificationCenter`, Windows WinRT toasts); tap → raise + open thread; `ClearForThread` on select |
| Desktop lifecycle | Minimize / focus-lost treated as background so banners can fire while the process is alive |
| Contracts | `SERVICE_ENDPOINTS.md`, `PLATFORMS.md` updated |
| `call_wake` | Opaque type + Android JNI + client fetch-then-ring (P008 / V006); relay emit rule documented |

## Ops (client)

| Item | Purpose |
|------|---------|
| `android/app/google-services.json` | Enables FCM source set in the Android build (provider-agnostic Firebase project) |
| Linux runtime | `libdbus-1` (linked at build via `libdbus-1-dev` / pkg-config) |

Relay operators configure their own FCM credentials on the server; those settings are not part of this repository.

## Follow-ups

- iOS / APNs
- FCM token refresh → automatic re-register after native start
- Android notification tap → open specific thread in ChatController (PendingIntent already embeds `thread_id`)
- Relay implementation of `call_wake` emit on call-invite path (contract ready; provider outside this tree)
