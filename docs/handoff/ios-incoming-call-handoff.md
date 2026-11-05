# iOS incoming-call UX — two issues (handoff for the CallKit / push owner)

Found while Kenneth was device-testing Mac → iPhone calls (2026-11-05). Diagnosis only; no call-media code was changed here.

## 1. Mic audio never flows after answering a VoIP-pushed call

**Symptom (Mac caller → iPhone callee):** Message/alert pushes work. A call shows the **native CallKit** incoming screen (via the VoIP push). Tapping **Answer** opens the Apple in-call UI, but the mic signal never reaches the call engine and the call never actually connects (no "connecting" handshake). The Mac keeps ringing.

**Traced flow** — `src/foundation/platform/CallSystemUi_Ios.mm`:
1. VoIP push → `pushRegistry:didReceiveIncomingPush…` → `OnCallPush` (L121): reports a **placeholder** incoming call to CallKit at once (iOS requires it), sets `pending_`, arms `kPendingRingNs`, `RequestUrgentWakeSync()`.
2. App wakes/syncs → the real call arrives → `ReportIncoming` (L74) adopts the `pending_` UUID (binds `call_id`).
3. Answer → `performAnswerCallAction:` → `OnAnswer` (L162): `ConfigureAudioSession()` sets the category, then `callbacks_.answer(call_id)` asks the engine to accept; `[action fulfill]`.

**Primary root cause:** `provider:didActivateAudioSession:` (**L306–310**) is a **no-op** (only `NSLog`). CallKit activates the `AVAudioSession` itself *after* the answer, and the app must **start/resume the call audio unit inside this callback**. Instead the mic/VPIO audio is started on the accept path, independent of CallKit's activation (`capabilities/media/src/lib/media/MediaDeviceArbiter.cpp:582` `core_->audio.Start()` → `capabilities/media/src/lib/media/VoiceProcessingIo_Apple.cpp` `AudioOutputUnitStart`). So the CallKit-owned session never carries the app's mic → "signal never reaches the mic", call never connects.
→ **Fix direction:** for a CallKit-answered call, start/resume the VPIO unit in `didActivateAudioSession:` and stop it in `didDeactivateAudioSession:` (L312), rather than on the accept callback.

**Secondary suspect (race):** In `OnAnswer`, if the user answers while still `pending_` (the ring not yet bound by sync), it calls `callbacks_.answer("")` with an **empty call_id** (L164–166). If the engine can't resolve which call to accept from an empty id, the accept is a no-op. Worth confirming the answer-before-ring-bound path.

**Files:** `src/foundation/platform/CallSystemUi_Ios.mm` (L121 OnCallPush, L162 OnAnswer, L306 didActivateAudioSession, L312 didDeactivateAudioSession); `capabilities/media/src/lib/media/MediaDeviceArbiter.cpp:582`; `capabilities/media/src/lib/media/VoiceProcessingIo_Apple.cpp` (AudioOutputUnitStart).

## 2. The PIN gate must not block answering an incoming call

**Requirement (Kenneth):** When a call comes in, the user should be able to **answer immediately**, without the PIN/unlock gate in the way. Require the PIN only *afterwards*, when the user leaves the call and goes into chat/content. Otherwise the call is often missed while the PIN is being entered.

On iOS the CallKit screen is already above the app's PIN gate, so answering happens on the system UI — the gap is **after** answering: returning to the app must land on the in-call screen, not behind the PIN gate. The in-call UI should be reachable while a call is active with the PIN gate deferred; content (chats etc.) still requires the PIN once the call ends.

This touches the unlock-gate ↔ active-call interaction. Flagging it here with (1) since it's the same incoming-call flow.
