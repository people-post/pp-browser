# Capability repos — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## P0 — Measure and gate

- [x] `scripts/check/check_capability_edges.py`: groups `src/` by capability ([DESIGN § Capabilities in the
      tree](DESIGN.md#capabilities-in-the-tree)), counts `#include` edges between groups (tests included), and
      fails on any edge against the order unless `capability_edges_allowlist.txt` holds it with exactly that
      count; fails on a directory under `lib/`, `domain/` or `feature/` no capability owns.
- [x] In CI's lint job beside the other `scripts/check/` guards.

## P1 — Cut the back edges (in-repo)

One PR per edge, lowering its allowlist line ([DESIGN § Edges against the order](DESIGN.md#edges-against-the-order)):

- [ ] Misplaced tests first (cheap): calls logic tests under `feature/conversations/tests` → `lib/calls/tests`;
      `common/chat` and `common/ui` tests under `common/tests` → beside their code.
- [ ] people → messaging (21), net → messaging (10), base → messaging (7)
- [ ] people → net (5): a port identity declares, the app or net implements
- [ ] messaging → app (5): messaging declares the ports AI and the shell implement (CR007)
- [ ] messaging → calls, messaging → node, node → messaging, node → calls, calls → broadcast

## P2 — Thin the base (in-repo, then pp-cpp-common / pp-cpp-crypto)

- [ ] Capabilities stop reading `foundation/data` (`Config`, `UserPreferences`, `MeshRole`) and
      `foundation/platform`: the app passes values and ports in.
- [ ] `Tr()` out of capability code (calls and messaging): error codes, text in the app.
- [ ] Runtime core into pp-cpp-common ([DESIGN § Runtime](DESIGN.md#runtime-core--pp-cpp-common)):
      registered owners instead of `OwnerThreadId::MediaSessions`, no product version defines; release
      pp-cpp-common and pin it. Does not wait for a second app.
- [ ] The crypto capabilities add today (message and attachment ciphers, PSK session store) into pp-cpp-crypto.

## P3 — Capability targets (in-repo)

- [ ] Each capability builds as its own CMake target set and links only what is below it (the gate's
      allowlist now empty).
- [ ] The namespace and include-path scheme for capabilities ([DESIGN § Open questions](DESIGN.md#open-questions)),
      renamed in-repo.

## Contracts repo (any time, CR010)

- [ ] Specs and golden vectors for formats with more than one implementation (posts, the account locker, the
      KEM binding) in their own repo; pp-browser and the TS services (app-posts, app-static-api) pin it and
      test against it instead of copied vectors.

## P4 — The capabilities repo (when a second app needs it, CR005 / CR006)

Not scheduled. Before it: one pin list per app and a sibling-build mode; one home for vendored deps (CR012).

- [ ] Create the capabilities repo (people-post, public) with the target sets that app needs, the gate, and
      their contracts and tests; release; pin in every app; pp-browser drops its copy.
- [ ] Expected first: pp-node as its own app (people, net, node; with `packaging/pp-node`, the N-* smokes,
      the hard lab and `pp_node_closure.sh`).
