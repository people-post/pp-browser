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
*Amended 2026-11-06 ([at-rest A013](../at-rest-crypto/DECISIONS.md#a013--pin-grace-window-a-pin-typed-a-short-while-ago-is-not-asked-again)):* a push wake inside the PIN grace window unlocks the vault through the grace entry; it never extends the window. Outside a window (or with it off) the rule above stands.

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
**Amended 2026-10-23:** a call (any phase but Idle) holds the work running in the background like a wake window does, and lets go 5 s after it ends (`AppLifecycle::SetCallInProgress`); the relay is polled every 2 s meanwhile. Before, a call went on in the background on media threads alone: the other side's hang-up reached the app only when the user opened it, and a call answered without opening the app (the lock screen; CallKit) went silent when the wake window closed.  
**Cross-link:** app lab `background_is_push_only`, `push_wakes_background_callee`, `minimized_still_rings`, `peer_hangs_up_while_backgrounded`, `answered_in_background` (`scripts/test/pp_app_lab.py`).

## P011 — Stay connected in the background (opt-in, Android)

**Date:** 2026-10-22  
**Status:** Accepted (product owner)  
**Decision:** Me → Profile → **Stay connected in the background** (`ProfilePreferences.stay_connected`, off by default, shown on Android). On: entering the background no longer pauses the app's work (`AppLifecycle::SetStayConnected`), the relay is polled every 30 s there (`kStayConnectedRelayPollIntervalMs`), a wake window never pauses it, and `PpConnectionService` — a foreground service (`specialUse`, with its subtype explained in the manifest) and its ongoing notification — keeps the process alive. The app still reads not attentive, so calls and messages notify.  
**Rationale:** Phones without a push service (the no-FCM build, de-Googled phones, most phones sold in China) otherwise get nothing in the background (P010) but WorkManager every 15 min — too slow for calls. Like Signal without Play services: the user chooses battery for reach. Vendor push (Huawei, Xiaomi, OPPO, vivo) may follow.  
**Cross-link:** app lab `stay_connected_rings_in_background`.

## P012 — Call invites are urgent; iOS rings by an alert push

**Date:** 2026-10-22  
**Status:** Accepted (product owner: alert push first, CallKit later)  
**Decision:** A call invite sent through the relay is marked `urgent` (`CallControlClient` → `send_control(…, urgent)` → `RelayEnvelope::relay_wake_urgent` → `"urgent": true` on `POST /v1/messages`). The flag is a hint beside the send record, not in its signature or the end-to-end envelope: the relay learns "this one should ring", never what it is or who calls. A relay that honours it (rate-limited per sender → recipient) wakes Android with `call_wake` and sends iOS a visible time-sensitive "Incoming call" alert (`loc-key`, worded on the device). Tapping it opens the app, which syncs and rings; in the foreground the app drops the alert (it rings anyway). iOS gets its own notifier (`IosLocalNotifier`) for what a silent wake syncs; its incoming-call banner and the alert share an id (`pp-call`). VoIP push + CallKit (a full-screen ring) may follow for builds where CallKit is allowed.  
**Rationale:** iOS throttles silent pushes and does not deliver them to an app the user swiped away, so a call invite could go unnoticed. An alert is always shown. Keeping the hint opaque and unsigned leaves older relays and older clients working unchanged; the sender could already wake the recipient with any message, so trusting the hint costs no more than the rate limit allows.  
**Cross-link:** `docs/contracts/SERVICE_ENDPOINTS.md` (push wake types); unit test `P2pRelayWireTest.UrgentWakeIsARelayHintOutsideTheEnvelope`.

## P013 — CallKit + VoIP push on iPhone (where allowed)

**Date:** 2026-10-23  
**Status:** Accepted (product owner: alert push first, CallKit later — this is the later)  
**Decision:** iPhone builds with `PP_BROWSER_IOS_CALLKIT` (on by default; off for builds sold in China, where App Store review rejects CallKit) ring incoming calls on CallKit's screen — full screen, on the lock screen too, answered or declined there.
- **Token:** PushKit gives a VoIP token, registered as a second device (`platform: ios_voip`, `device_id: <ios device id>.voip`). The relay sends that phone's urgent wakes (P012) to it as a VoIP push instead of the alert, and sends it nothing else.
- **Push:** a VoIP push puts "Incoming call" on the CallKit screen at once, since iOS requires a call for every VoIP push, and wakes the app to sync. The app fills that call in with the ring it finds: caller name, video.
- **Bridge:** `CallSystemUiBridge` (feature/calls, unit-tested) keeps the CallKit screen in step with `CallPresenter`'s.
  - A ring goes there when the app is not in the foreground, or when a push's call is waiting.
  - Answered there, it stays there; answered in the app, CallKit lets it go.
  - Declined or hung up there, the presenter declines or leaves.
  - Answered or declined before its ring was known (the app still waking, or PIN-locked, P003), the choice applies to the ring once it shows, within 60 s counted from the unlock (messaging up) when that came later, and never later than 3 min after the choice. Until then the push's call stays on CallKit's screen (an unanswered one for 75 s, an answered one up to 3 min) and the PIN screen reads "Enter your PIN to connect your call."; after the unlock it waits 20 s (45 s once answered) for its ring, which is then accepted at once.
  - Audio of a call answered there starts when CallKit activates the audio session (`didActivateAudioSession`), not before: `AudioSession::SetSystemManaged` makes `ActivateForVoip` wait for it (3 s at most) and leaves the release to CallKit.
  - The app's own ringtone stays quiet while CallKit rings.
- **Privacy:** a generic handle, and no calls in the phone's recents or iCloud call history.

**Rationale:** the alert (P012) is easy to miss and needs a tap and an unlock before the call can be answered. A call should ring like a call.  
**Depends on:** a call holds the app's work running in the background (P010 amendment, 2026-10-23) — a call answered on the lock screen goes on with the app never opened.  
**Cross-link:** `CallSystemUi.h`, `CallSystemUi_Ios.mm`, `call_system_ui_bridge_test.cpp`; relay `planPushWakes` (www).

## P014 — Android rings by its own incoming-call notification; a push reaches a stopped app

**Date:** 2026-11-05  
**Status:** Accepted (product owner: notification permission, push for a killed app, full-screen call notification)  
**Decision:** Android gets the phone's own call screen that P013 gave iPhones, built from what Android has: an incoming-call notification (`PpCallNotifications.java`, native `AndroidCallSystemUi`, the same `ICallSystemUi` and `CallSystemUiBridge`).
- **Ring:** a ring the app does not show itself (in the background, or a push's call waiting) goes to the notification: the calls channel's ringtone until it is answered or gone (the app's own ringtone stays quiet), full screen on a locked phone, Answer / Decline on it (the system's call style from Android 12; plain buttons before, or where the system refuses it). Decline works on the lock screen; Answer opens the app (Android asks for the phone's unlock first). The app is not shown over the lock screen: the call, and the chats behind it, wait for the phone's unlock.
- **Push with the app stopped:** the push starts the process, not the app. A `call_wake` rings "Incoming call" (no caller: the push carries none, P001) and its full-screen notification starts the app, which syncs and fills the call in with the ring it finds. This is the push's call of P013, with its timeouts: 75 s unanswered while the app cannot find rings (not running, or PIN-locked, P003), 20 s (45 s answered) once it can, never more than 3 min. Answered or declined before the ring is known, the choice applies to the ring, as on CallKit. With the app running in the background the same "Incoming call" rings at once, and the app is woken to sync; in front, the app rings itself. An `inbox_wake` with the app stopped shows a generic "New message" (P003's wording), unless Me → Show notifications is off; the setting is kept for this beside the app (`ILocalNotifier::SetMessageBannersOn`). The app running, it syncs and shows what came, as before.
- **Token:** the app asks Firebase for its current token at each start. `onNewToken` alone came once, mostly before the native side was loaded, and was lost.
- **Permission:** Android 13+ asks for notifications at the app's start, as iOS does.

**Rationale:** on Android a call to a phone whose app was swiped away, or killed by the system, rang nowhere (the wake reached no native code), and a ring in the background was an ordinary banner. A call should ring like a call, and a stopped app is the common case on phones.  
**Follow-ups:** an in-call foreground service (microphone in the background — done: p2p-av-calls V061); Android's Telecom (`ConnectionService`) for the system's call routing and Bluetooth answer.  
**Cross-link:** `PpCallNotifications.java`, `AndroidCallSystemUi.cpp`, `CallSystemUiBridge` (`call_system_ui_bridge_test.cpp`), P013.
