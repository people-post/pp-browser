# Push notifications — design

## Threat model

| Adversary | Capability | Mitigation |
|-----------|------------|------------|
| FCM / APNs | Sees device token, timing, opaque data map | No plaintext; `type=inbox_wake` or `type=call_wake` only |
| Relay | Already sees ciphertext envelopes + who talks to whom | Push adds only “device may be woken”; no extra body |
| Commercial SaaS | Aggregated opens/clicks/segments | **Rejected** — no SaaS (P002) |
| Local attacker with locked vault | Background wake | No auto-unlock; generic notify or silent defer (P003) |

## Alerts vs sync (P005)

**Show notifications** controls:

1. Whether `ILocalNotifier` may post OS banners
2. Whether an FCM token is registered with the relay

Inbox sync remains enabled while the process (or WorkManager job) can run. There is no v1 toggle to disable all background network.

## Opaque wake payload

FCM **data** message (not notification payload):

```json
{
  "type": "inbox_wake"
}
```

Call ring (P008 / V006):

```json
{
  "type": "call_wake"
}
```

Optional non-identifying bookkeeping keys may be added later; never `thread_id`, `call_id`, contact names, or message body.

## Sync schedule

| Mode | Sync | OS alert |
|------|------|----------|
| Foreground | 2s (`kForegroundRelayPollIntervalMs`) | In-app only |
| Background, alerts on, alive | FCM wake + 30–60s poll | Yes (detail or generic) |
| Background, alerts on, Android dead | FCM + WorkManager backup 1–6h | Yes |
| Background, alerts off, alive | 30–60s poll | No |
| Background, alerts off, Android dead | WorkManager ~15 min | No |
| Desktop minimized | 30–60s poll | Iff alerts on |

Constant: `kBackgroundRelayPollIntervalMs = 45000`.

## Client flow

1. Peer → relay `POST /v1/messages` (ciphertext).
2. If recipient has registered device token(s) → provider best-effort FCM data send.
3. Client `SyncInboxFromWake` → signed `PollInbox` → `RelayReceivePipeline`.
4. If `!IsForeground()` and alerts on → `ILocalNotifier`; else unread/badge only.

## PIN / vault

Background wake/poll must not unlock the DEK. If vault locked and alerts on → generic “New message”. If alerts off → no UI.

## HTTP (relay provider)

| Endpoint | Purpose |
|----------|---------|
| `POST /v1/devices/register` | `{ platform, push_token, device_id }` + relay-api-v1 signature |
| `POST /v1/devices/unregister` | Remove token |

## Components

| Component | Role |
|-----------|------|
| `NotificationPrefs` | `show_notifications` (default true) |
| `ILocalNotifier` | OS banners |
| `IPushDeviceRegistrar` | Platform FCM token |
| `IPushDeviceClient` | Signed HTTP register/unregister |
| `BackgroundSyncScheduler` | Interval + WorkManager policy |
| `MeshMessagingService::SyncInboxFromWake` | Poll ingest outside UI tick |
