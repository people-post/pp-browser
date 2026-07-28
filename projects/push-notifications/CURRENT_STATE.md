# Push notifications — current state

**Last updated:** 2026-07-24

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
- **`call_wake`** opaque type for [p2p-av-calls](../p2p-av-calls/) a1 (V006) — distinct from `inbox_wake`; client fetch-then-ring; no `call_id` in payload
