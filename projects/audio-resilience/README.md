# Audio resilience

**Status:** Planned — design written 2026-11-20; no phase started. Starts once the ongoing projects are done.  
**Owner:** Hongwei + agents  
**Stable refs:** [CALLS.md](../../docs/architecture/CALLS.md), [THREADING.md](../../docs/architecture/THREADING.md), [TESTING.md](../../docs/architecture/TESTING.md), [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md)  
**Related:** [p2p-av-calls](../p2p-av-calls/) (V060 hold, the call's audio), [media-client-layers](../media-client-layers/) (L016: devices with budgets, per-session media), [hard-lab](../hard-lab/), [call-path-resilience](../call-path-resilience/) (the same idea for the network path)

## One-line goal

Test the **class** of failures where the OS or the audio hardware takes, breaks or holds a call's audio, not one incident: a matrix of disruptions × moments × who owns the session, checked against five invariants (recovery, honesty, clean slate, no hostage devices, control never blocks), in unit tests and in the app lab.

## Why now

iPhone, 2026-11-19, a 1:1 call placed in the app (not on CallKit): the caller switched to another app and played a video. The far voice faded and went; it never came back after the video closed. The other side could not hear the caller any more, and after a redial still could not, while the caller heard them. Force-quitting the app fixed it.

Nothing in our tests would have caught it. The lab's 127 scenarios cover the network and signalling thoroughly, but no test checks that the other side can **hear**, nothing in a test plays the part of the phone's audio system, and nothing checks what one call leaves behind for the next. Details: [DESIGN.md § Coverage today](DESIGN.md#coverage-today).

## Scope (v1)

| In | Out |
|----|-----|
| A hearing check: a known signal in, a per-person heard level out (gtest and app lab) | Audio quality (MOS, echo, loudness) |
| The five invariants as checks, in gtest and in the app lab | Automating real phones (device farms, XCUITest) |
| A device backend that injects faults at open / read / write / close | Video and the camera (a later round may reuse the matrix) |
| An emulator of the phone's audio system (iOS session, Android audio focus) behind a seam in `AudioSession` | Changing iOS behaviour itself; CallKit for calls placed in the app (a product change, tracked apart) |
| Lab commands for OS events; seeded random sequences in the lab nightly | Desktop audio devices beyond what the fault backend models |
| A short device check list for what only a phone can show | |

## Documents

| File | Tier | Purpose |
|------|------|---------|
| [DESIGN.md](DESIGN.md) | Spec | Coverage today, the model, the invariants, the building blocks, the emulator's rules and their evidence |
| [DECISIONS.md](DECISIONS.md) | ADRs | AR001– |
| [PHASES.md](PHASES.md) | Order | ar0–ar6 checklists |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Status | Only place for phase status |
