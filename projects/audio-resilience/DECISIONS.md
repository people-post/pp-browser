# Audio resilience — decisions

Prefix **AR**. Status lives in [CURRENT_STATE.md](CURRENT_STATE.md); spec in [DESIGN.md](DESIGN.md).

---

## AR001 — Test the class, not the incident

**Date:** 2026-11-20  
**Status:** Accepted as direction (Hongwei, 2026-11-20); the details open until ar1  
**Context:** 2026-11-19, iPhone: after another app's video took a call's audio, the call never recovered, and every later call sent silence until the app was force-quit. Getting that device's log and fixing what it shows is cheap, but it fixes one path from one symptom; the reasons nothing caught it — no hearing check, no OS in any test, nothing checking what a call leaves behind — would remain.  
**Decision:** Cover failures where the OS or the audio hardware takes, breaks or holds a call's audio as a class: a matrix of disruptions × moments × who owns the session ([DESIGN § The model](DESIGN.md#the-model)), checked against five invariants (I1 recovery, I2 honesty, I3 clean slate, I4 no hostage devices, I5 control never blocks) at the cheapest tier that can host each case. Incidents feed the matrix (§ Findings) instead of getting one test each.  
**Rationale:** The incident broke four invariants at once; tests for one symptom would have missed the others. Invariants checked over random sequences find what nobody wrote a scenario for, as the network chaos suites do for paths.  
**Alternatives rejected:** Reproduce the incident from the device log and add its regression only (fast, narrow; leaves the class open). Rely on device dogfood (finds these, but late and without a way to replay).

---

## AR002 — The lab reproduces a model of the OS, every rule with its evidence

**Date:** 2026-11-20  
**Status:** Proposed  
**Decision:** The platform audio emulator (B4) is a model. Each of its rules is written down with its evidence — Doc, Code (a device log showed it), or Assumed ([DESIGN § The emulator's rules](DESIGN.md#the-emulators-rules-and-their-evidence)). An Assumed rule is on the device check list (ar6) until a device confirms or corrects it. A device observation that disagrees with a rule changes the rule first, then the tests.  
**Rationale:** The bugs worth finding sit where our model of iOS differs from iOS. A lab that silently encodes our assumptions would pass exactly where the product fails; written evidence makes the model's weak spots visible and checkable.  
**Alternatives rejected:** An emulator without stated rules (unreviewable fidelity); device-only testing for OS behaviour (no replay, no CI).

---

## AR003 — Hearing is measured, not inferred from packets

**Date:** 2026-11-20  
**Status:** Proposed  
**Decision:** Tests that claim audio works check that a known tone sent by one side is present in what the other side decodes (B1), per sender. Packet and frame counters (`rx_audio_frames`, `tx_audio_frames`) stay as diagnostics, never as proof of hearing.  
**Rationale:** The engine sends a frame every packet length whatever happens (silence while interrupted, muted or without a mic — by design, V067), so counters stay healthy on a call nobody can hear. That is the state of the 2026-11-19 redial.  
**Alternatives rejected:** A non-zero decoded level (noise and comfort sounds pass); counting non-silent Opus packets (says the sender sent speech, not that the receiver played it).

---

## AR004 — Split AudioSession into a portable core and an OS port

**Date:** 2026-11-20  
**Status:** Proposed  
**Decision:** The session logic in `AudioSession_Ios.mm` (activation, resume and retry, the CallKit-owned path, voice clips, deactivation) moves into portable C++ over an OS port (session calls in, notifications out). The port has two implementations: AVFoundation on iOS and the emulator everywhere. Android's session and focus follow the same shape second. The split lands in small steps, each behaviour-neutral on device, after B1–B3 are in place to watch it.  
**Rationale:** The iOS logic is the part that decides recovery and has no test at all; it only builds for iOS. `ResumeRetryChain` showed the pattern works here. Doing it after the hearing check and the fault backend means the refactor itself is checked by the new tests.  
**Alternatives rejected:** Test on the iOS simulator (its audio session does not reproduce interruptions faithfully, and CI has no iOS test job); emulate one level lower by faking AVFoundation symbols (brittle, Objective-C only).

---

## AR005 — What needs no refactor comes first

**Date:** 2026-11-20  
**Status:** Proposed  
**Decision:** Order the work so value lands before the delicate part: the hearing check (B1), the cross-call ledger (B2) and the fault backend (B3) first — they need no change to `AudioSession_Ios.mm` — then the emulator (B4), then lab OS events and random sequences (B5, B6). [PHASES.md](PHASES.md) follows this.  
**Rationale:** B1–B3 already cover I3, I4, I5 and the device-fault half of the matrix, and they are the safety net for AR004's split.
