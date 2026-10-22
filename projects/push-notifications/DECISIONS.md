# Push notifications — decisions

## P001 — Opaque wake only

**Date:** 2026-07-13  
**Decision:** FCM/APNs payloads carry `type=inbox_wake` (data message). No message text, contact names, or thread ids.  
**Rationale:** Aligns with E2E threat model; OS vendors and the relay must not learn content via push.

## P002 — No commercial push SaaS

**Date:** 2026-07-13  
**Decision:** The configured relay talks to FCM and APNs directly. No OneSignal / Firebase Console product analytics as source of truth.  
**Rationale:** Aggregators see engagement stats across the user base; conflicts with privacy product stance.

## P003 — PIN-locked generic notify

**Date:** 2026-07-13  
**Decision:** Background sync must not unlock the vault. Alerts on + locked → generic “New message”. Alerts off + locked → silent.  
**Rationale:** Avoids decrypt-without-consent and PIN bypass via wake path.

## P004 — Desktop local-only v1

**Date:** 2026-07-13  
**Decision:** Desktop uses local OS notifications + in-process background poll. No remote desktop push while process is dead.  
**Rationale:** Desktop always-on wake is out of scope; process-alive UX is enough for v1.

## P005 — Alerts ≠ sync

**Date:** 2026-07-13  
**Amended 2026-10-22 (P010):** the device stays registered for push with alerts off — in the background a phone is reached by push only, and calls ring and notify with alerts off too. **Show notifications** hides message banners only.  
**Decision:** **Show notifications** controls OS banners and FCM registration only. Inbox sync stays on via slow poll / WorkManager when alerts are off. Unregister FCM when alerts off.  
**Rationale:** Users who silence banners still want unread catch-up; unregister keeps provider push send paths honest.

## P006 — Background poll + WorkManager fallback

**Status:** In-process background poll superseded by [P010](#p010--background-is-push-only-a-wake-is-a-window) (it never ran: background pauses the coordinator its timer lives on). WorkManager stays.  
**Date:** 2026-07-13  
**Decision:** Implement D032 background path: in-process poll at `kBackgroundRelayPollIntervalMs` (45s) while alive; Android WorkManager ~15 min when process dead and alerts off; rarer backup (1–6h) when alerts on.  
**Rationale:** Push is wake-only; poll remains source of truth. Fast background poll is rejected for battery/Doze.

## P007 — iOS deferred

**Date:** 2026-07-13  
**Decision:** Interfaces may mention `platform: ios`; APNs implementation after Android E2E.  
**Rationale:** Android is shipped/m milestone 2; iOS is reserved.  
**Status:** Superseded by p8 (2026-10-12) — `IosPushDeviceRegistrar` + www `ApnsPushSender` landed.

## P009 — APNs silent wake + .p8 on relay

**Date:** 2026-10-12  
**Decision:** iOS uses APNs HTTP/2 silent pushes (`content-available`, `apns-push-type: background`) with opaque `type=inbox_wake|call_wake`. Auth is a Team .p8 JWT on www (`BRF_WWW_APNS_*`); client stores hex device token via `platform=ios`. Sandbox host only when `useSandbox` is set (USB/dev).  
**Rationale:** Matches P001 opacity; keeps secrets off-device; same register API as FCM.

## P008 — `call_wake` opaque type

**Status:** Relay emit not implementable as written (the control type is end-to-end encrypted); superseded for Android / desktop by every ring notifying itself (P010). `call_wake` stays accepted by the client.  
**Date:** 2026-07-28  
**Decision:** FCM/APNs may carry `type=call_wake` in addition to `inbox_wake`. Same opacity rules as P001 (no `call_id`, names, or media). Client treats `call_wake` as force sync then **fetch pending call invites → ring UI** (V006). Vault-locked behavior follows P003 with generic “Incoming call” copy when a banner is shown. Relay emits `call_wake` on call-invite class envelopes (see `SERVICE_ENDPOINTS.md`).  
**Rationale:** Prioritize ring UX over chat badge without leaking call metadata to the push path.  
**Alternatives:** Overload `inbox_wake` only (weak ring); put `call_id` in FCM (rejected).

## P010 — Background is push only; a wake is a window

**Date:** 2026-10-22  
**Status:** Accepted (product owner: push only in the background, for power) — supersedes P006's in-process background poll  
**Decision:** In the background a phone app's work is paused (`AppLifecycle::OnWillEnterBackground`) and it has no relay poll of its own: it is reached by push. A push (`inbox_wake` / `call_wake`) or a WorkManager run resumes the work for `kBackgroundWakeWindowMs` (20 s) — enough to sync the inbox and ring — then the work pauses again unless the app came to the foreground; a newer wake extends the window. WorkManager stays as the safety net for missed pushes. A minimized desktop window is not background (it keeps working and notifies).  
**Rationale:** The OS enforces it anyway (iOS suspends, Android Doze batches network), and the messengers people compare against work this way. The 45 s in-process poll never ran (its timer lived on the paused coordinator), and a push could not wake the app (its sync was posted to that paused coordinator); once that was fixed, one push kept the phone awake for good.  
Every ring raises the "Incoming call" notice (`CallPresenter`), however its invite arrived; the platform's notifier shows it only when the user is not looking. So the relay needs no call class (it could not see one: the control type is end-to-end encrypted).  
**Cross-link:** app lab `background_is_push_only`, `push_wakes_background_callee`, `minimized_still_rings` (`scripts/test/pp_app_lab.py`).

## P011 — Stay connected in the background (opt-in, Android)

**Date:** 2026-10-22  
**Status:** Accepted (product owner)  
**Decision:** Me → Profile → **Stay connected in the background** (`ProfilePreferences.stay_connected`, off by default, shown on Android). On: entering the background no longer pauses the app's work (`AppLifecycle::SetStayConnected`), the relay is polled every 30 s there (`kStayConnectedRelayPollIntervalMs`), a wake window never pauses it, and `PpConnectionService` — a foreground service (`specialUse`, with its subtype explained in the manifest) and its ongoing notification — keeps the process alive. The app still reads not attentive, so calls and messages notify.  
**Rationale:** Phones without a push service (the no-FCM build, de-Googled phones, most phones sold in China) otherwise get nothing in the background (P010) but WorkManager every 15 min — too slow for calls. Like Signal without Play services: the user chooses battery for reach. Vendor push (Huawei, Xiaomi, OPPO, vivo) may follow.  
**Cross-link:** app lab `stay_connected_rings_in_background`.
