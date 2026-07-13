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
| Brief (www) | `/v1/devices/register|unregister`, Mongo `relay_push_devices`, FCM wake on message accept |
| Android | FCM (opt-in via `google-services.json`), WorkManager, NotificationCompat, JNI wake/token |
| Contracts | `SERVICE_ENDPOINTS.md`, `PLATFORMS.md` updated |

## Ops

| Env / file | Purpose |
|------------|---------|
| `BRF_FCM_PROJECT_ID` | Firebase project id |
| `BRF_FCM_SERVICE_ACCOUNT_JSON` | Service account JSON path or inline JSON |
| `android/app/google-services.json` | Enables FCM source set in Android build |

## Follow-ups

- iOS / APNs
- FCM token refresh → automatic re-register after native start
- Notification tap → open specific thread in ChatController
