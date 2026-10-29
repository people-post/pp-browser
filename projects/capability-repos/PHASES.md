# Capability repos — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## P0 — Measure and gate

- [ ] `scripts/check/check_capability_edges.py`: groups `src/` by capability (the path table in
      [DESIGN § Sizes](DESIGN.md#sizes-kloc-non-test)), counts `#include` edges between groups, and fails on
      any edge not in an allowlist. Start the allowlist at today's edges, so the gate only stops new ones;
      each later phase shrinks it.
- [ ] Run it in `scripts/check/` with the others (CI and local).

## P1 — Cut the back edges (in-repo)

One PR per pair, in whichever direction is the thin one ([DESIGN § edges](DESIGN.md#capability--capability-include-edges)):

- [ ] people → messaging (12), net → messaging (5), pricing → messaging (1)
- [ ] messaging → calls (2), calls → broadcast (2)
- [ ] messaging → AI (2): AI declares the ports it needs; messaging implements them or the app wires them
- [ ] node → messaging (1), node → calls (1), messaging → node (2)
- [ ] net → people (2)
- [ ] the stray messaging → app/gui include

## P2 — Thin the base (in-repo, then pp-cpp-common / pp-cpp-crypto)

- [ ] Capabilities stop reading `foundation/data` (`Config`, `UserPreferences`, `MeshRole`) and
      `foundation/platform`: the app passes values and ports in.
- [ ] `Tr()` out of capability code (32 sites in calls and messaging): error codes, text in the app.
- [ ] Runtime core into pp-cpp-common ([DESIGN § Runtime](DESIGN.md#runtime-core--pp-cpp-common)):
      registered owners instead of `OwnerThreadId::MediaSessions`, no product version defines; release
      pp-cpp-common and pin it.
- [ ] The crypto capabilities add today (message and attachment ciphers, PSK session store) into pp-cpp-crypto.

## P3 — Capability targets (in-repo)

- [ ] Each capability builds as its own CMake target set and links only what is below it (enforced by P0's
      gate, now with an empty allowlist for cross-capability edges).

## P4 — Repos, leaves first

Each: move code, tests and contracts; release; pin in every app; pp-browser drops its copy.

- [ ] node (smallest; already bounded by pp-node's link allowlist; take `packaging/pp-node`, N-* smokes, the
      hard lab and `pp_node_closure.sh` with it)
- [ ] identity (people + net + pricing)
- [ ] AI
- [ ] messaging
- [ ] realtime (calls + broadcast)
