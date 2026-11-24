# Task scopes: current state

**As of:** 2026-11-20

**Proposed, nothing built.** The design is written ([DESIGN.md](DESIGN.md)). [TS001](DECISIONS.md#ts001--every-posted-task-belongs-to-a-task-scope)
is proposed, for Hongwei's review, with open questions Q1–Q10, each with a recommendation. The work is planned for
after release 1. T0 and T1 could land earlier (Q9).

Baseline at `origin/main` 197eb3a9d, counted with a first-line grep (T1 replaces it with exact counts):

- 246 raw-`this` / `&` `AppRuntime` post or schedule sites in 33 non-test files, out of 400 `AppRuntime` post or
  schedule calls in 61 files;
- `ConversationsHub.cpp` has 45 such sites and 7 bound to `pp::Lifetime`;
- `ConversationsHub.h` has 18 mutexes and 7 atomics.

Related, outside this project: PR #676 (open) fixes `HubParticipantTest`'s use-after-free with a test-only drain.
T2 makes that drain unnecessary.

Next: Hongwei's review of TS001. Then T0 ([PHASES.md](PHASES.md#t0--taskscope-in-the-runtime-with-tests)).
