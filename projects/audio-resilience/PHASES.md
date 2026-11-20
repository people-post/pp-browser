# Audio resilience — phases

Ordering and checkboxes only. **Status:** [CURRENT_STATE.md](CURRENT_STATE.md). **Spec:** [DESIGN.md](DESIGN.md).

```
ar0 (docs) ── ar1 (hearing + invariants) ──┬── ar2 (fault matrix, gtest) ──┐
                                            └── ar3 (AudioSession split + emulator) ── ar4 (lab OS events) ── ar5 (random sequences)
ar6 (device check list) runs alongside from ar1; promotion to docs/ at the end of each phase
```

ar2 and ar3 can run in parallel once ar1 is in. ar3 is the delicate phase (AR004): small, behaviour-neutral steps.

## ar0 — Design (this folder)

- [x] README / DESIGN / DECISIONS (AR001–AR005) / PHASES / CURRENT_STATE
- [x] Coverage inventory and the four gaps (DESIGN § Coverage today)
- [x] Findings from 2026-11-19 recorded (DESIGN § Findings)
- [ ] Review: the matrix, the invariants' times, the emulator's rules

**Exit:** the direction and the first phase agreed.

## ar1 — Hearing check and the invariants (B1, B2)

- [ ] Synthetic tone mic endpoint (a frequency per user) as a backend for gtests; `pp-app-probe --synthetic-audio`
- [ ] Heard read-out per remote stream (level, tone present, since) in `MediaEngineHealth` and the probe's `state`; no audio content leaves the process (DIAGNOSTICS.md)
- [ ] Fix I1 / I2 times from what the code can promise; write them in DESIGN
- [ ] Cross-call ledger read-out (`AudioSession`, arbiter, engine retirements) for tests; probe `audio_state`
- [ ] gtest helpers: `ExpectHears(a, b, within)`, `ExpectCleanSlate()`, `ExpectNoHostageDevices(within)`
- [ ] App lab: the hearing check in the existing call scenarios (`first_contact`, `background_mid_call`, `redial_storm`, `group_leave_keeps_audio`, …) — a scenario that passed on counters and fails on hearing is a finding
- [ ] App lab: I3 after every scenario's hang-ups

**Exit:** every call scenario in the lab checks hearing both ways and a clean slate.

## ar2 — Device-fault matrix in gtest (B3)

- [ ] Scriptable fault backend: per kind (mic, speaker, voice pair) × operation (open, read, write, close) × behaviour (fail, hang until let go, silence, slow) × from step
- [ ] Matrix D6 × M3–M7 × {separate devices, voice pair}, each against I3, I4, I5
- [ ] Bring in `media_engine_redial_test` (I4: an ended call's stuck thread) and decide fix 1 (`LetGo`, local branch `test/ios-redial-mic-held`) with the matrix's results in hand
- [ ] Each red case: a finding row in DESIGN, then its fix in the owning project

**Exit:** the matrix runs in CI; every case is green or a recorded finding.

## ar3 — AudioSession split and the platform emulator (B4)

- [ ] OS port interface (session calls in; interruption / route / reset / app state notifications out; current route)
- [ ] Move activation and deactivation into the portable core (behaviour-neutral on device; checked on a phone)
- [ ] Move resume and the retry chain's driver
- [ ] Move the CallKit-owned path (SetSystemManaged / NotifySystemActivation, `TryResume` under CallKit)
- [ ] Emulator with rules R1–R10, each citing its evidence (AR002)
- [ ] gtests: D1–D5 × M1–M7 × O1/O2 against I1–I5, in-process (two engines, one emulated phone each)
- [ ] Android: focus and session through the same shape (second)

**Exit:** the iOS session logic runs in Linux CI against the emulator; the device behaves as before the split.

## ar4 — Lab OS events (B5)

- [ ] Probe commands: `other_app_audio`, `os_call`, `siri`, `route`, `media_reset`, `suspend` / `resume` (SIGSTOP / SIGCONT), `revoke_mic`, `audio_state`
- [ ] Scenarios: one per disruption × the moments that matter (start with D1 × M3, D1 + D5 × M3 → M7 — the 2026-11-19 sequence — and D2 without "ended")
- [ ] Tags (`audio`) in `app_lab_select.py`; the path → tag table covers `lib/media`, `AudioSession*`, `CallSystemUi*`

**Exit:** the 2026-11-19 sequence runs as a lab scenario with its invariants checked.

## ar5 — Seeded random sequences (B6)

- [ ] Generator over the axes with weights; `--seed`; replay
- [ ] Invariant checks after every step with their time budgets
- [ ] Minimiser (drop steps while it still fails); a failing seed becomes a named scenario
- [ ] Nightly in the lab beside chaos / soak; a bounded in-process run in gtest

**Exit:** a nightly run of N sequences, failures triaged into scenarios.

## ar6 — Device check list (alongside)

- [ ] A written list of what only a phone shows: rules R4–R7, a second mic endpoint beside a wedged one (fix 1), the tone through voice processing, CallKit vs app-owned calls under another app's video
- [ ] Run it on an iPhone and an Android phone before ar3's split lands and after; record results against each rule (AR002)

## Promotion

- [ ] TESTING.md / TEST_STRATEGY.md: the audio invariants and where each is checked; the hearing check as the proof of "audio works"
- [ ] CALLS.md: the invariants the call's audio promises (I1–I5), once they hold
- [ ] Mark AR decisions superseded by the docs they land in
