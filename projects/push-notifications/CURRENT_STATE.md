# Push notifications — current state

**Last updated:** 2026-11-09

## Landed (wave 1 + iOS)

| Area | State |
|------|-------|
| Project docs | `projects/push-notifications/` |
| Prefs | `ProfilePreferences.show_notifications` (Me → Profile select) |
| Platform | `ILocalNotifier`, `IPushDeviceRegistrar`, `BackgroundSyncScheduler`, desktop + Android + iOS notifiers |
| Net | `HttpPushDeviceClient` + relay-api device sign ops 3/4 |
| Messaging | `SyncInboxFromWake`, background unread → OS notify when alerts on |
| Relay contract | Client calls `/v1/devices/register|unregister`; expects opaque wakes from the provider when tokens are registered |
| iOS sounds | `assets/sounds/call.caf` (29 s) and `msg.caf` (short), made by `scripts/ui/gen_ringtone.py`, bundled at `Resources`; local notifications use them; relay `aps.sound` (`call.caf` for `call_wake`, `msg.caf` for message alerts) follows (#563; default sound until then) |
| Android | FCM (opt-in via `google-services.json`; token asked for at each start), WorkManager, NotificationCompat, JNI wake/token; incoming-call notification via `CallSystemUiBridge` (P014): rings, full screen when locked, Answer / Decline; a push with the app stopped rings "Incoming call" / shows "New message"; notification permission asked at start (13+). Verify on a device after the 2026-11-06 fix: Answer on the notification connects and its hang-up still reaches the call; one ring only |
| iOS | APNs token via `IosPushDeviceRegistrar` + `IosApnsAppDelegate` (+load hook adding callbacks to SDL's scene delegate); silent `inbox_wake` → `BackgroundSyncScheduler`; the relay's `call_wake` Time Sensitive alert is the only call path (P019: no PushKit, no CallKit): foreground → call wake sync, no banner; tap → PIN if locked → sync → the app's own ring (or the missed call's chat once the invite is expired); `IosLocalNotifier` posts banners after a wake (tap → open thread); a tap carries `wake_hint` and routes to its ring / message / missed-call summary after unlock (P015, P018) |
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
| `BRF_WWW_APNS_USE_SANDBOX` | USB/dev tokens only (`api.sandbox.push.apple.com`); unset on hosted relays, which then reach TestFlight / App Store builds only |

## Follow-ups

- APNs token refresh → automatic re-register after native start (Android asks FCM for its token at each start, P014)
- Android (P014): verify on a device — killed-app call push (full screen, answer before unlock), decline on the lock screen, Android 14 full-screen permission; in-call foreground service; Telecom
- Android notification tap → open specific thread in ChatController (PendingIntent already embeds `thread_id`)
- Relay (www): honour `urgent` — `call_wake` to Android, the time-sensitive alert to iOS, rate limit (P012)
- iOS alert tap (P015): verify on a device — tap a call alert and a message alert from the lock screen, PIN, landing on the right chat / ring
- iOS call alert (P019; needs app-static-api's alert-only calls deployed and an App ID with Time Sensitive Notifications): verify on a device —
  - locked phone: the alert breaks through Focus, a tap asks for the PIN, then the app rings the call (voice or video answer), audio both ways
  - expired invite (tap after about a minute): lands in the caller's chat on the missed-call record
  - app open and unlocked: no banner, the app's own ring
  - ringing is a single alert tone, silenced by the silent switch (accepted, P019)
  - decline in the app: the caller sees it declined; opening the app does not ring that call again
  - two calls close together: a tap opens the tapped call's ring, not the other
  - the signing profile includes `com.apple.developer.usernotifications.time-sensitive` before the last commit of the branch is built
  - a stranger's call posts the alert before anything is decrypted (accepted, P019)
- Call pushes older than the invite's lifetime (about 65 s) are not filtered at receipt any more: the tap finds the missed-call record instead
- 2026-11-09 clean-up: the leftovers of P016 / P017 (the push's named call, "the push's call is over", `CallPushAge`, the CallKit mute / end-reason plumbing) are deleted; Android's push names no call. The relay should drop legacy `ios_voip` bindings (SERVICE_ENDPOINTS)
