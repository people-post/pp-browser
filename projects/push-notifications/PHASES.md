# Push notifications — phases

## p0 — Project docs

- [x] README, DESIGN, CURRENT_STATE, DECISIONS, PHASES
- [x] Register in `projects/README.md`

## p1 — Contracts + prefs + stubs

- [x] `kBackgroundRelayPollIntervalMs` in MessagingLimits
- [x] `show_notifications` on ProfilePreferences
- [x] `ILocalNotifier`, `IPushDeviceRegistrar`, `IPushDeviceClient`
- [x] PlatformServices wiring

## p2 — Relay provider gateway

- [x] Device register / unregister HTTP contract consumed by client
- [x] Provider best-effort FCM data send on message accept (out of client tree)

## p3 — BackgroundSyncScheduler

- [x] Foreground 2s / background 45s while alive
- [x] Bounded IO resume for background sync
- [x] `P2pMessagingService::SyncInboxFromWake`
- [x] Android WorkManager worker (JNI)

## p4 — Android FCM E2E

- [x] Firebase Messaging (opt-in `google-services.json`)
- [x] Token JNI + wake JNI
- [x] NotificationCompat gated by alerts pref
- [x] Token register/unregister via PushDeviceCoordinator

## p5 — Desktop local

- [x] `DesktopLocalNotifier`
- [x] Background/minimized poll + alert when unfocused
- [x] Native OS banners (Linux D-Bus, macOS UNUserNotificationCenter, Windows WinRT)
- [x] Tap → raise window + open `thread_id`; `ClearForThread` on select

## p6 — Settings UI

- [x] Me → Profile **Show notifications**
- [x] Toggle wires unregister/register

## p7 — Promote

- [x] SERVICE_ENDPOINTS.md devices section
- [x] PLATFORMS.md background poll + WorkManager
- [x] Refresh CURRENT_STATE / README status

## Later — call ring wake

- [x] Opaque **`call_wake`** type ([p2p-av-calls V006](../p2p-av-calls/DECISIONS.md#v006--call_wake-push-shape)); contract + client fetch-then-ring; relay emit on call invite path (P008)
