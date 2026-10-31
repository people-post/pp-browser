# Capability repos — current state

**Updated:** 2026-10-30  
**Phase:** P0 done; P1 next

## Done

- Measured sizes, capability edges and base usage ([DESIGN § Where we are](DESIGN.md#where-we-are-measured-2026-10-30-develop-at-0d77a6a35)).
- Direction revised (CR005–CR012): move only for a second app, into one capabilities repo; AI stays in the
  app; identity is people, net its own, pricing inside messaging; posts + content are "social"; contracts and
  vectors in their own repo.
- **P0:** `scripts/check/check_capability_edges.py` in CI's lint job, starting from 11 edges / 67 include
  sites against the order (`capability_edges_allowlist.txt`).

## Next

1. P1: the misplaced tests first, then one edge per PR.
2. P2: runtime core into pp-cpp-common.
3. Contracts repo (CR010) when convenient.
