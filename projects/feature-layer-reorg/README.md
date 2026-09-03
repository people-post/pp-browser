# Feature / app layer reorg

**Status:** **f7v1 done** — `src/gui/` above feature ([F008](DECISIONS.md#f008--gui-layer-above-feature))  
**Owner:** Hongwei + agents  

**Stable refs:** [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md), [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md), [RUNTIME_COMPOSITION.md](../../docs/architecture/RUNTIME_COMPOSITION.md), [`src/feature/README.md`](../../src/feature/README.md)  
**Prerequisite:** `src/base/` → `foundation` + `domain` (**done**). This project continues with `src/feature/`, `src/app/`, and the planned **`src/gui/`** layer.

## One-line goal

Shrink and clarify `feature/` and `app/` by moving **sure** engines into domain, carving oversized folders only when ownership is clear, lifting product UI into **`gui`** above feature, and treating the final module map as a **working North Star** until peels prove it.

## Approach (locked method + locked names, phased paths)

| Lock now | Keep revisable / phased |
|----------|-------------------------|
| Layer litmus; domain peer independence; sure peels first | When folders physically rename |
| Vocabulary + end-state names ([F007](DECISIONS.md#f007--vocabulary--end-state-feature-names)) | Exact bands under `gui/` |
| Calls: nested band first ([F004](DECISIONS.md#f004--calls-home-nested-band-first-then-top-level)) | Top-level `pp_feature_calls` timing |
| No top-level `feature/chat` | `ChatController` class rename |
| Product UI layer = **`gui`** above feature ([F008](DECISIONS.md#f008--gui-layer-above-feature)) | When `feature/ui` physically moves |

See [NORTH_STAR.md](NORTH_STAR.md) and [DECISIONS.md](DECISIONS.md) (F001–F008).

## Why this order

1. **Foundation/domain peels taught ownership.** Feature still holds stores and pure helpers that already match domain litmus.
2. **Folder splits without peels just move the god-objects.** `MessagingHub` stays hard until mass drops; UI nesting (f5) was staging only.
3. **Presenters are not feature peers.** `gui` above feature matches [UI_FUNCTIONAL_BOUNDARY](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md); name **`gui`** avoids clash with `domain/ui`.

## Scope

| In | Out (unless later expanded) |
|----|-----------------------------|
| Lower clear stores/policy/helpers feature → domain | Product behavior / wire format changes |
| Document working module hypotheses | Rewriting call SMs or chat UX |
| Mechanical folder splits when ownership is proven | Domains linking each other |
| Lift `feature/ui` → `src/gui` | Naming the GUI layer `src/ui/` |
| App wirer splits (still composition root) | New frameworks; presenter registries for their own sake |
| Update include guards / CMake as modules move | “Clean up everything” mega-PRs |

## Documents

| File | Purpose |
|------|---------|
| [NORTH_STAR.md](NORTH_STAR.md) | Working target map — revise as peels teach |
| [PHASES.md](PHASES.md) | f0–f7 delivery order (sure → structural → gui lift) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the tree looks like today vs gaps |
| [DECISIONS.md](DECISIONS.md) | Method + naming ADRs (F001–F008) |
| [CANDIDATES.md](CANDIDATES.md) | Peel / split inventory with confidence tags |

## Promote when settled

When a phase ships durable layout rules, update:

- [`docs/architecture/SRC_LAYOUT.md`](../../docs/architecture/SRC_LAYOUT.md) (layers + feature/gui tables)
- [`src/feature/README.md`](../../src/feature/README.md) / new `src/gui/README.md`
- Include guard scripts under `scripts/`

Do **not** leave the only description of shipped layout under `projects/`.
