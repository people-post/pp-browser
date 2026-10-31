# Capability repos — current state

**Updated:** 2026-10-30  
**Phase:** P0 done; P1 under way; P2's runtime core done

## Done

- Measured sizes, capability edges and base usage ([DESIGN § Where we are](DESIGN.md#where-we-are-measured-2026-10-30-develop-at-0d77a6a35)).
- Direction revised (CR005–CR012): move only for a second app, into one capabilities repo; AI stays in the
  app; identity is people, net its own, pricing inside messaging; posts + content are "social"; contracts and
  vectors in their own repo.
- **P0:** `scripts/check/check_capability_edges.py` in CI's lint job, starting from 11 edges / 67 include
  sites against the order (`capability_edges_allowlist.txt`).
- **P1, misplaced tests:** calls logic tests to `lib/calls/tests`, `common/chat` and `common/ui` tests beside
  their code — 10 edges / 53 sites left.
- **P2, runtime core:** in pp-cpp-common v1.3.0 (`common/runtime/`), owners registered by the app
  (`foundation/runtime/ProductOwners.h`).

## Next

1. P1: one edge per PR.
2. P2: `foundation/data` / `platform` / `Tr()` out of capability code; crypto additions into pp-cpp-crypto.
3. Contracts repo (CR010) when convenient.
