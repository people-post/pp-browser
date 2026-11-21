# Audio resilience — current state

**As of:** 2026-11-20

Design written ([DESIGN.md](DESIGN.md), AR001–AR005); the direction (AR001) agreed with Hongwei 2026-11-20. **No phase started**: work begins once the ongoing projects are done. Next: review of the matrix, the invariants' times and the emulator's rules (ar0's last box), then ar1.

## Landed

| Item | State |
|------|-------|
| ar0 docs | README / DESIGN / DECISIONS / PHASES / CURRENT_STATE |

## Related work outside this folder

| Item | State |
|------|-------|
| Fix 1 prototype: an ended call's stuck audio thread no longer keeps the mic (`MediaDeviceArbiter::LetGo` at the slow-join warning) with `media_engine_redial_test` and two arbiter tests | Draft PR [#680](https://github.com/people-post/pp-browser/pull/680), marked to re-evaluate in ar2 before it leaves draft (its limits: DESIGN § Findings) |
| CallKit for calls placed or answered in the app | Not started; a product decision for [p2p-av-calls](../p2p-av-calls/) |
| The 2026-11-19 device's log | Not collected |
