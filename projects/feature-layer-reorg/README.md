# Feature / app layer reorg

**Status:** **f3 landed** — f4v1 nested calls band next  
**Owner:** Hongwei + agents  

**Stable refs:** [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md), [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md), [RUNTIME_COMPOSITION.md](../../docs/architecture/RUNTIME_COMPOSITION.md), [`src/feature/README.md`](../../src/feature/README.md)  
**Prerequisite:** `src/base/` → `foundation` + `domain` (**done**). This project continues with `src/feature/` and `src/app/`.

## One-line goal

Shrink and clarify `feature/` and `app/` by moving **sure** engines into domain, carving oversized folders only when ownership is clear, and treating the final module map as a **working North Star** until peels prove it.

## Approach (locked method + locked names, phased paths)

| Lock now | Keep revisable / phased |
|----------|-------------------------|
| Layer litmus; domain peer independence; sure peels first | When folders physically rename |
| Vocabulary + end-state names ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names)) | Exact f5 `shell` vs `ui/shell` |
| Calls: nested band first ([F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)) | Top-level `pp_feature_calls` timing |
| No top-level `feature/chat` in the end state | `ChatController` class rename |

See [NORTH_STAR.md](NORTH_STAR.md) and [DECISIONS.md](DECISIONS.md) (F001–F007).

## Why this order

1. **Foundation/domain peels taught ownership.** Feature still holds stores and pure helpers that already match domain litmus.
2. **Folder splits without peels just move the god-objects.** `MessagingHub` and `feature/ui` stay hard to reason about until mass drops.
3. **A premature North Star fights evidence.** Early peels (e.g. `ContactReachability` → people) change which includes and modules make sense.

## Scope

| In | Out (unless later expanded) |
|----|-----------------------------|
| Lower clear stores/policy/helpers feature → domain | Product behavior / wire format changes |
| Document working module hypotheses | Rewriting call SMs or chat UX |
| Mechanical folder splits when ownership is proven | Domains linking each other |
| App wirer splits (still composition root) | New frameworks; presenter registries for their own sake |
| Update include guards / CMake as modules move | “Clean up everything” mega-PRs |

## Documents

| File | Purpose |
|------|---------|
| [NORTH_STAR.md](NORTH_STAR.md) | Working target map — revise as peels teach |
| [PHASES.md](PHASES.md) | f0–f6 delivery order (sure → structural) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the tree looks like today vs gaps |
| [DECISIONS.md](DECISIONS.md) | Method + naming ADRs (F001–F007) |
| [CANDIDATES.md](CANDIDATES.md) | Peel / split inventory with confidence tags |

## Promote when settled

When a phase ships durable layout rules, update:

- [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) (feature table + migration step 5)
- [`src/feature/README.md`](../../src/feature/README.md) (module map + link order)
- Include guard scripts under `scripts/`

Do **not** leave the only description of shipped layout under `projects/`.
