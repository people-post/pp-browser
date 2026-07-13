# Push notifications — current state

**Last updated:** 2026-07-13

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
| Contracts | `SERVICE_ENDPOINTS.md`, `PLATFORMS.md` updated |

## Ops (client)

| Item | Purpose |
|------|---------|
| `android/app/google-services.json` | Enables FCM source set in the Android build (provider-agnostic Firebase project) |

Relay operators configure their own FCM credentials on the server; those settings are not part of this repository.

## Follow-ups

- iOS / APNs
- FCM token refresh → automatic re-register after native start
- Notification tap → open specific thread in ChatController
