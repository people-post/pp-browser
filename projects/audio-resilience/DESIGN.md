# Audio resilience — design

**Phase status:** [CURRENT_STATE.md](CURRENT_STATE.md) only. **Order:** [PHASES.md](PHASES.md). **Rationale:** [DECISIONS.md](DECISIONS.md).

## Overview

A call's audio passes through three things we do not control: the phone's audio system (iOS's audio session, Android's audio focus and routes), the audio hardware behind it, and the app's own lifecycle (inactive, background, suspended). Each can take the audio away, give it back, refuse it, or stall a thread that is using it. This project tests that whole class, not one incident ([AR001](DECISIONS.md#ar001--test-the-class-not-the-incident)):

1. **Measure hearing** — a known signal goes in on one side and a heard level comes out on the other, so a test can tell "the other side hears me" from "packets flow" ([AR003](DECISIONS.md#ar003--hearing-is-measured-not-inferred-from-packets)).
2. **State the invariants** that must hold whatever happens (§ [Invariants](#invariants)).
3. **Play the OS and the hardware** in tests: a fault-injecting device backend, and an emulator of the phone's audio system behind a seam in `AudioSession` ([AR002](DECISIONS.md#ar002--the-lab-reproduces-a-model-of-the-os-every-rule-with-its-evidence)).
4. **Cross the matrix** — disruptions × moments × who owns the session — in written scenarios and in seeded random sequences, checking the invariants after every step.

## Goals

- Every invariant is checked in a gtest and in the app lab, for every row of the disruption list that the tier can host.
- The iOS session logic (`AudioSession_Ios.mm`: resume, retries, activation, the CallKit path) runs in tests on Linux for the first time.
- A failure found by the random sequences becomes a named scenario and, where it can, a gtest ([TESTING.md § When a higher tier finds a bug](../../docs/architecture/TESTING.md#when-a-higher-tier-finds-a-bug)).
- What only a real phone can show is a short, written device check list, not a gap nobody names.

## Non-goals

- Audio quality (MOS, echo, loudness, latency budgets): this project asks "is it heard", not "how well".
- Automating real phones.
- Fixing the OS or deciding product changes (CallKit for calls placed in the app, § [Findings](#findings-to-feed-in)) — the tests make their need visible; the change is decided where it belongs.
- Video and the camera in v1 (the camera has budgets already, L016; the matrix can take it later).

## Coverage today

Inventory 2026-11-20 (origin/main).

**Network and signalling: covered thoroughly.** The app lab (`scripts/test/pp_app_lab.py`, `pp-app-probe` users over the real lab relay) has 127 scenarios: redial storms, crossed calls, restarts while ringing, a peer vanishing, push and background (`background_mid_call`, `answered_in_background`, …), NAT, network drops, relay churn, group calls.

**Audio: covered as isolated rules.** Unit tests in `capabilities/media/src/lib/media/tests/`:

| Area | Tests |
|------|-------|
| The interruption flag and its end handlers | `audio_session_interruption_test` (8): the flag, handlers per engine, silence while interrupted, a reopen waits for the end, voice processing tried again |
| iOS retry rules | `resume_retry_chain_test` (16): the chain's ticks and results — pure, no session |
| Android audio focus | `audio_focus_interruption_test` (7) |
| Android route changes, SDL rebuilds | `audio_route_recovery_test` (17), `audio_reset_handoff_test` (6) |
| A starved mic | `capture_starve_policy_test` (4) |
| A frame every packet length | `media_engine_liveness_test` (6) |
| Device budgets | `media_device_arbiter_test` `StuckDeviceTest` (opens / closes past their budget) |

### The four gaps

1. **Nothing checks that the other side hears.** The lab's audio is SDL's dummy driver (silence in, nothing out, `app_probe/main.cpp`), and `rx_audio_frames` counts silent packets too. Every scenario passes while packets flow; what users notice — "I can't hear you" — is the one thing nothing checks.
2. **The phone's audio system is never in a test.** `AudioSession_Ios.mm` has no test at all: it builds only for iOS, and CI builds iOS only for TestFlight. On Linux `AudioSession` is empty stubs (`AudioSession_Default.cpp`). The lab can say `background` / `foreground`, but not "another app took the audio", "a phone call came in", "Siri", "AirPods went away" or "media services reset".
3. **Nothing checks what one call leaves behind for the next.** Process-wide state can survive a hang-up (§ [Cross-call state](#cross-call-state)). One unit test touches any of it (`ClearInterruption` at a call's end).
4. **Device faults are covered for their budgets, not their side effects.** An open or close past its budget is covered; a read or write that blocks, a mic that goes quiet while the OS has the session, an open refused in the background, are not — and those leave media threads stuck, which is where side effects come from.

## The model

Three axes. A test case is one point of their product (or a sequence of points).

### Disruptions

| Id | Disruption | Platforms |
|----|------------|-----------|
| D1 | The OS takes the audio: another app's media (non-mixable), a phone call, a CallKit call of another app, Siri, an alarm | iOS (interruption), Android (focus loss: transient, for good) |
| D2 | The take ends: with an "ended", without one (Siri; iOS 17+ `route_disconnected`), while the app is in the background, or never | iOS, Android |
| D3 | A route change: Bluetooth or wired comes / goes, the speaker toggled, a car | iOS, Android |
| D4 | Media services reset (iOS `AVAudioSessionMediaServicesWereReset`) | iOS |
| D5 | App lifecycle: inactive (Control Center, a banner, Face ID), background, suspended, killed and relaunched | iOS, Android |
| D6 | Device faults: an open fails, an open hangs, a read / write blocks, a close hangs, a device gives silence | all |
| D7 | The mic permission revoked mid-call | iOS, Android |

### Moments

M1 ringing (in or out) · M2 connecting · M3 in the call · M4 during a device reopen · M5 during the hang-up · M6 between calls · M7 at a redial.

### Who owns the session

O1 the app (iOS calls placed or answered in the app; desktop) · O2 CallKit (iOS calls answered on the call screen) · O3 Android audio focus.

### Invariants

Checked after every step of a scenario or sequence. Times are proposals, fixed in ar1.

| Id | Invariant | Measured by |
|----|-----------|-------------|
| **I1 Recovery** | Once a disruption is over (the take ended, or the app is active again and nothing else holds the audio), both sides hear each other again within **3 s** | The hearing check (B1), both directions |
| **I2 Honesty** | While a side cannot send or play audio, both screens say so (on hold, `call.status.on_hold_*`; or the "no audio" state) within **2 s**; there is no silent dead call | Call surface `media_status` / roster `on_hold` vs the hearing check |
| **I3 Clean slate** | After a hang-up, the next call behaves as the first call after launch: the same devices, voice processing, no interruption or hold carried over | The cross-call state ledger (B2) compared with a fresh process |
| **I4 No hostage devices** | A call that has ended holds no device, past a bounded time (today 2 s, `kJoinStageWarnAfter`) | `MediaDeviceArbiter::Holders` |
| **I5 Control never blocks** | Hang-up, redial, answer and the UI respond within their budgets whatever the hardware does (L016) | Command round-trip times in the probe; the owner threads' watchdogs |

The 2026-11-19 incident breaks I1, I2 (no on-hold shown while the far side heard nothing), I3 and I4 at once.

### Cross-call state

What can outlive a call today (I3's ledger). Each entry gets a "fresh?" read-out for tests.

| State | Where |
|-------|-------|
| The interruption flag | `AudioSessionInterruptions.cpp` `g_interrupted` |
| Session in use, its generation, voice in use, voice playback | `AudioSession_Ios.mm` `g_session_active`, `g_session_gen`, `g_voice_active`, `g_voice_playback` |
| CallKit owns the session / CallKit activated it | `AudioSession_Ios.mm` `g_system_managed`, `g_system_active` |
| The retry chain | `AudioSession_Ios.mm` `RetryChain()` |
| The speaker choice | `AudioSession_Ios.mm` `g_speakerphone`; Android's equivalent |
| Tone holds | `AudioSessionHolds.cpp` |
| The last route change | `AudioSessionInterruptions.cpp` `g_route_changed_ms` |
| Who holds which device; lanes stuck | `MediaDeviceArbiter` holders, lanes |
| The engine's audio session generation | `MediaEngine.cpp` `g_audio_session_gen` |
| SDL's own idea of the iOS session | `SDL_coreaudio.m` `session_active` (sticks once set: `COREAUDIO_CloseDevice` counts the closing device as open, § [Findings](#findings-to-feed-in)) |
| A stuck rebuild (Android) | `SdlAudioReset` `SdlAudioResetSupported` turning false |
| Retired sessions still cleaning up | `MediaEngine` `retirements_` |

## Building blocks

| Id | Block | Tier | Needs the `AudioSession` split? |
|----|-------|------|------|
| B1 | Hearing check | gtest + app lab | No |
| B2 | Cross-call state ledger | gtest + app lab | No |
| B3 | Fault-injecting device backend | gtest (+ lab via a backend factory) | No |
| B4 | Platform audio emulator | gtest + app lab | **Yes** |
| B5 | Lab commands for OS events | app lab | Partly (B4 for D1–D4) |
| B6 | Seeded random sequences | app lab (nightly), gtest (bounded) | Best with B4 |

### B1 — Hearing check

- **Signal in:** a synthetic mic endpoint that plays a known tone (one frequency per user, so a group call can tell who is heard), the audio counterpart of `SyntheticVideo`. The probe takes it with a flag, like `--synthetic-video`; gtests use it as a backend.
- **Heard out:** per remote stream, the decoded level and whether the tone is present (a Goertzel filter at the sender's frequency, cheap and certain), over a short window. Exposed in `MediaEngineHealth` and the probe's `state` (`heard: {identity: {level, tone, since_ms}}`).
- **Never content:** the check reads levels and one frequency's energy; nothing of the audio leaves the process (DIAGNOSTICS.md).
- Muting, hold and silence frames read as "not heard", which is what I2 compares against.

### B2 — Cross-call state ledger

- A test-only read-out of every entry in § [Cross-call state](#cross-call-state) (for example `AudioSession::StateForTest()`, plus `MediaDeviceArbiter::Health`, the engine's retirements), and a probe command `audio_state`.
- I3 check: after any hang-up, once retirements are done (bounded), the ledger equals a fresh process's.

### B3 — Fault-injecting device backend

- An `IMediaDeviceBackend` whose endpoints follow a script: per kind (mic, speaker, voice pair) and per operation (open, read, write, close), **fail**, **hang until let go**, **give silence**, **slow by N ms**, from a step on.
- The first instance exists: `media_engine_redial_test.cpp` (draft [#680](https://github.com/people-post/pp-browser/pull/680)) wedges the mic read during an interruption and shows the redial refused the mic (I4).
- The matrix in gtest: D6 × M3–M7 × the separate-devices and voice-pair paths, each checked against I3, I4, I5.

### B4 — Platform audio emulator

The seam ([AR004](DECISIONS.md#ar004--split-audiosession-into-a-portable-core-and-an-os-port)): `AudioSession_Ios.mm` splits into

- a **portable core** — the session's state, resume and retry, activation, the CallKit-owned path, voice clips, deactivation with its busy retries — in C++, built everywhere; and
- an **OS port** — `setCategory` / `setMode` / `setActive` / `overrideOutputAudioPort`, the notifications in (interruption began / ended with reason, route change, media services reset, app state, protected data), the current route — implemented once on AVFoundation (iOS) and once by the emulator.

`ResumeRetryChain` is already split this way. Android's `AudioSession_Android.cpp` / `AudioFocusInterruption` gets the same treatment second (focus requests and grants, losses, the communication device).

The emulator is a scriptable model of the phone's audio system: who holds the audio (us, "another app", a phone call), what `setActive` answers given the app's state, which notifications it sends and when, and what our endpoints do meanwhile (the fault backend, B3, follows the emulator: an interrupted session's units give nothing).

### B5 — Lab commands for OS events

`pp-app-probe` commands that drive the emulator and the lifecycle:

| Command | Plays |
|---------|-------|
| `other_app_audio start\|stop [mixable]` | another app's media in front (D1, D2) |
| `os_call start\|end [answer]` | a phone call over ours (D1) |
| `siri start\|end` | a take with no "ended" (D2) |
| `route bt_on\|bt_off\|wired_on\|wired_off\|speaker` | D3 |
| `media_reset` | D4 |
| `inactive` / `background` / `foreground` (exist) / `suspend` / `resume` | D5; `suspend` is `SIGSTOP` of the probe process, `resume` `SIGCONT` (a suspended app runs nothing, its sockets age) |
| `revoke_mic` | D7 |
| `audio_state` | B2's ledger |

### B6 — Seeded random sequences

- A generator draws steps from the three axes (with weights: D1–D2 and D5 most), runs them against two (or three) users in a call, and checks I1–I5 after every step with its time budget.
- The seed is printed and replayable (`--seed`); a failing seed is minimised (drop steps while it still fails) and committed as a named scenario.
- Nightly in the lab (beside chaos / soak); a bounded version (a few hundred steps, in-process, B3 + B4) in gtest.

## The emulator's rules and their evidence

The emulator reproduces a **model**; a test that passes against it shows our code handles the model ([AR002](DECISIONS.md#ar002--the-lab-reproduces-a-model-of-the-os-every-rule-with-its-evidence)). So every rule carries its evidence, and a device observation that disagrees changes the rule first, then the tests.

Evidence levels: **Doc** (Apple / Android documentation), **Code** (our code relies on it and a device log showed it), **Assumed** (believed; to verify on a device — ar6 check list).

| # | Rule (iOS) | Evidence |
|---|------------|----------|
| R1 | A foreground app activating a non-mixable session interrupts an app-owned (O1) PlayAndRecord session: "began" is sent and our audio units stop | Doc |
| R2 | "Ended" is not guaranteed (Siri sends none; iOS 17+ `route_disconnected` sends none) | Doc + Code (B217, `AudioSession_Ios.mm` reason 4) |
| R3 | A background app activating a non-mixable session fails (`CannotInterruptOthers`) | Doc |
| R4 | An app in the background cannot start recording (a mic unit start or SDL mic open fails) | Assumed |
| R5 | An app with the `audio` background mode is suspended soon after it stops playing and recording; a suspended app runs no timers or threads | Doc (background audio) / Assumed (timing) |
| R6 | A CallKit-owned (O2) session has phone-call priority: another app's media does not interrupt it | Assumed |
| R7 | After a non-call interruption of a CallKit session, CallKit does not send `didActivateAudioSession` again | Assumed — decides whether `TryResume`'s CallKit path can ever resume |
| R8 | Media services reset drops every session setting; the app must set them again | Doc |
| R9 | `setActive:NO` while a voice-processing unit runs fails `IsBusy` | Code (`DeactivateAttempt`) |
| R10 | Opening an SDL device rewrites the session's category and options (`DefaultToSpeaker`, `DuckOthers`, …) and toggles `setActive` | Code (B36; `SDL_coreaudio.m` `UpdateAudioSession`) |

Android rules (focus loss transient / for good, no focus back after a loss for good, route change disconnects an open recording stream, SDL's AAudio default device dying) come from `AudioFocusInterruption.h` and `SdlAudioReset.h`, which already state them with their device evidence.

## Tiers

| Question | Tier |
|----------|------|
| One invariant against one fault or OS event, in one engine | gtest (B1–B4) |
| The matrix and short random sequences | gtest (bounded B6) |
| Two real app stacks over the real relay; lifecycle and suspension; UI honesty (I2) | app lab (B1, B2, B5, B6), Docker |
| Whether the model is right (R4–R7) and real hardware | device check list (ar6), by hand |

## Findings to feed in

Found while looking into 2026-11-19; recorded here so the tests cover them. None is decided by this project.

| Finding | Where | Covered by |
|---------|-------|-----------|
| An ended call's audio thread that never returns keeps the mic from every later call (the leases are released only after the joins, and the join has no limit). Prototype fix: `MediaDeviceArbiter::LetGo` at the slow-join warning — draft [#680](https://github.com/people-post/pp-browser/pull/680), not merged; open question: a second mic endpoint beside a wedged one on iOS | `MediaEngine.cpp` `FinishRetired`, `JoinThreadWatched` | B3, I4 |
| `TryResume` under CallKit never calls `setActive:YES`; it trusts `g_system_active`, which a non-call interruption may leave stale either way (R7) | `AudioSession_Ios.mm` `TryResume`, `ActivateForVoip` | B4, I1 (O2) |
| Every voice-processing open calls `ActivateForVoip`, bumping the session generation (a resume in flight then reads Stale) | `VoiceProcessingIo_Apple.cpp` `Open` | B4, B6 |
| SDL's `session_active` sticks once set: `COREAUDIO_CloseDevice` updates the session while its own device still counts as open | pp-cpp-ui `third_party/sdl3/.../SDL_coreaudio.m` | B4 (R10), I3 |
| A retired session's `Deactivate` is skipped when a newer session activated since its retire began; `Deactivate` is the only path that clears the interruption flag besides a resume | `MediaEngine.cpp` `ReleaseAudioSessionIfUnchanged` | B2, I3 |
| Calls placed or answered in the app are never on CallKit, so any app's media interrupts them (R1); CallKit would likely prevent it (R6) | `CallSystemUi_Ios.mm` (no `CXStartCallAction`) | I1 for O1 vs O2; the product change is decided in p2p-av-calls |

## Open questions

- I1's 3 s and I2's 2 s: right for a phone in the background coming back? Fix in ar1 from what the code can promise.
- Where the hearing check's tone sits so voice processing (echo cancellation, AGC) and Opus do not remove it — on the desktop path there is no voice processing; on a device the check is by ear (ar6).
- Whether B4's split is one change or several (activation first, then resume, then CallKit), given how delicate `AudioSession_Ios.mm` is (its fixes so far: B174, B217, B230).
