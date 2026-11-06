# Proposal: call waiting between PP calls (Hold & Accept, Switch)

**For:** Hongwei (decision). **From:** Kenneth. **Status:** proposal, 2026-11-06. No code yet.

**Summary**
- Today a second PP call ringing during a call offers only **End & Accept / Ignore**. Phones let you answer the second call and keep the first on hold; we propose the same: **Hold & Accept**, then a **Switch** row to flip between the two calls.
- The held person sees the existing V060 "On hold" pill; we reuse `on_hold` in the roster, so no new control message is expected. Still one call with media at a time (V055 kept).
- 1:1 calls only in v1 (group calls keep End & Accept). Rough size: client UI 3-4 days; your side (hold a call's path without tearing it down) is the open part.
- **Decision needed:** do it / not now / do it differently (see the alternative at the end).

<details>
<summary>Details</summary>

## Today
- `docs/architecture/CALLS.md` row "Conflict (2nd invite while outbound/in-call)": conflict copy `End & Accept` / `Ignore`; Accept leaves the other local call first; single active call; the active call stays shown over the second ring.
- `CallSessionWorkflow::CheckCanStartCall` refuses to start (outbound) while an invite is pending ("Decline the incoming call first").
- V055 decision 3: calls may ring several at once, only one has media. The seat (`CallMediaSeat`) is an exclusive lock only; each call's `CallMedia` owns its own paths (V056); `LiveCalls` holds one `LiveCall` per open call.
- V060: `on_hold` / `video_paused` ride the roster. `CallController::SyncLocalHold` sets it from `AudioSession::IsInterrupted()` and calls `CallSessionManager::SetLocalOnHold`, which publishes for `call_media_.MediaRunningCallId()` (the one running call). Our 2026-11-06 change shuts the held side's mic and camera.

## Proposed UX
- A ring during a call offers **Hold & Accept / End & Accept / Ignore** (1:1 active call + 1:1 invite only).
- Hold & Accept: call A is marked held locally and in its roster (`on_hold`), call B is accepted and gets the media. A's peer sees "On hold on {name}'s side" and the pill (V060 item 4/5), not "Reconnecting".
- In call B the screen shows a **Switch** row ("{name} - on hold"). Tap: B goes on hold, A resumes. Ending the active call offers to resume the held one.
- Still one call with media; we do not mix two calls.
- Outbound start while holding: stays refused (unchanged).

## Protocol impact
- New control message: **not expected.** `on_hold` in the roster is what the other side already shows. Difference from V060: today it is OS-initiated and for the running call; here the user holds a call that is not the running one, so the publish needs an explicit call id (small API change on `SetLocalOnHold`).
- Held 1:1 media path - three options, **to confirm with Hongwei**:
  1. Keep the session and the path, stop mic/camera/playback (our side), keep the 500 ms heartbeat. Cheapest; the path stays warm; resume is instant. Needs the seat to allow "held" per call.
  2. Keep the session, drop the media path, re-anchor on resume (direct, then hop). Saves battery and relay load; resume takes seconds.
  3. Hold on the signalling only (no media for A at all).
  Our guess: 1 for short holds, 2 after a timeout (e.g. 60 s). Whether the heartbeat or the media key (V003 rotation) behaves with a silent, held side: **to confirm**.
- Whether the seat (one exclusive epoch) can be handed from call A's `CallMedia` to call B's while A keeps its keys and path state: **to confirm** (V036/V056 suggest yes, not verified in code).

## Split
- **Ours (client):** ring/conflict UI and copy (en + zh-Hans), Switch row, presenter state for "held call", `CallController` hold on user action (not only OS interruption), mic/camera shut on the held call, tests.
- **Yours (calls/network):** `LiveCalls` / `CallMedia` hold two open calls with exactly one running media; the seat handoff; the per-call hold publish; held-path policy (above); the invite/accept order (accept B must not make `CheckCanStartCall`-style refusals).

## Risks
- Battery/relay: a warm held path (option 1) costs heartbeat and a relay slot for the hold's duration; needs a timeout.
- Keys/paths of two calls alive together: stale call-id checks, rotation of the held call's media key while held, hop reservation for A while B is running.
- The OS audio session is per app: Hold & Accept must not trigger our own V060 interruption logic for the hold (separate flag from `AudioSession::IsInterrupted`).
- Group calls excluded in v1 (many paths, roster of N, rejoin rules V053).
- Peer on an older app ignores `on_hold` and may show "no audio"/"Reconnecting" after a while.

## Size (rough)
- Client: 3-4 days plus device checks (Mac, iPhone, Android).
- Calls side: 3-5 days, mostly option 1 plus the seat/handoff; option 2 adds a few more.

## Alternative: not doing it
Keep End & Accept; improve copy only: say who is calling and that the current call will end, and offer to call back the person we ended with (a callback row in chat). Zero protocol work, about half a day.

</details>
