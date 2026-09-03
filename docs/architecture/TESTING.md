# Testing doctrine (tiers, layers, doc homes)

**Tier:** architecture

How we design for testability and choose where coverage lives. **Ops inventory** (purpose IDs, CI ladder, compose sets, scripts) lives in [TEST_STRATEGY.md](../ops/TEST_STRATEGY.md). Layer map: [SRC_LAYOUT.md](SRC_LAYOUT.md).

---

## Goal

Cover **risk-weighted functional behaviors** — outcomes, invariants, and known failure modes — at the **cheapest sufficient tier**.

This is **complete risk accounting**, not line-coverage at every tier. A behavior may be intentionally incomplete at a given tier when a cheaper or more expensive tier already owns it (see [Skip taxonomy](#skip-taxonomy)).

---

## Decision rules

1. **Cheapest layer that answers the question** — pick the purpose, then the cheapest tier that can prove it.
2. **Push complexity down** — prefer seams in `common` / `foundation` / `domain` that unit tests can almost fully cover. If an integration path is too expensive or flaky to own, first ask whether a real lower-layer seam would make the behavior unit-testable.
3. **Higher tiers verify wiring and environment** — they do not re-prove codec, SM, or store rules already covered below.
4. **Extract only for real product boundaries** — do not invent test-only “libs.” Push into foundation/domain (or owned `src/lib/` / FetchContent stacks) when the logic is a coherent engine two features could share; leave genuine composition, lifetimes, and packaging at feature/app/smoke.

---

## Tiers

| Tier | Answers | Cost / flake |
|------|---------|--------------|
| **Unit** | Rules, codecs, state machines, stores, parsers, policy | Lowest |
| **Integration** | Typical valuable paths + known conflict/teardown modes across real stacks on one machine (in-process / loopback) | Medium |
| **Smoke** | Packaging, multi-process, deploy reachability, env/caps — cases cheaper tiers cannot see | Highest |

**Kinds of risk** (orthogonal to tiers):

| Kind | Question |
|------|----------|
| **Correctness** | Right outcomes / state transitions |
| **Reliability / soak** | No hang, leak, or deadlock over time |
| **Capacity / SLO** | How much before quality or success rate collapses |

Hard filter (networking): if a failure mode reproduces with in-process loopback, it does **not** belong in multi-node smoke — see [TEST_STRATEGY.md](../ops/TEST_STRATEGY.md).

---

## Map to source layers

| Tier | Best home | Proves | Must not try to prove |
|------|-----------|--------|------------------------|
| **Unit** | `common`, `foundation`, `domain/*`, pure parts of `feature` | Module behavior in isolation | Real multi-process topology, packaging, NAT |
| **Integration** | `feature` composition; in-process multi-host / loopback under `domain`/`feature` tests | Valuable product paths with real collaborators | Every GUI chrome path; capacity curves |
| **Smoke** | Packaged binaries, Docker/real peers, thin probes (`pp-call-probe`, `pp-node-probe`) | Boot, env/caps, process isolation | Detailed branch coverage of domain logic |

Default stress vehicle is **not** full GUI. Prefer thin clients / in-process compose; reserve GUI for sparse chrome checks (`B-UI` in ops inventory).

**Per-layer expectation:**

- **`common` / `foundation` / `domain`** — aim for near-full unit coverage of owned rules and engines.
- **`feature`** — integration for typical + known-bad paths; if a path is too hard, prefer extracting a domain/foundation seam over growing a mega-suite.
- **`gui` / `app`** — sparse unit/chrome where useful; smoke for packaging and multi-process product journeys.

---

## Skip taxonomy

When a behavior is not covered at a tier that “could” host it, record why (in the colocated ledger or purpose inventory):

| Reason | Meaning |
|--------|---------|
| `covered-below` | Cheaper tier already owns the proof |
| `covered-above` | Only meaningful as smoke (packaging, NAT, multi-process) |
| `glue-gap` | Needs Tier B / product glue before claiming the path |
| `cost/flake` | Deferred to nightly/manual; not PR-blocking |
| `non-goal` | Explicitly out of scope for now |

Every high-risk behavior should have a **named home tier and evidence**, or an **explicit skip**.

---

## Where documentation lives

**One editable home per fact.** Everything else is a pointer.

| Content | Home |
|---------|------|
| Doctrine (this file): tiers, push-down, skips, layer map | [`docs/architecture/TESTING.md`](TESTING.md) |
| CI ladder, purpose IDs (`N-*` / `B-*` / `N-HARD-*`), compose PR set, scripts, soak/chaos | [`docs/ops/TEST_STRATEGY.md`](../ops/TEST_STRATEGY.md) |
| Hard lab topology / scenario ladder (Tier C deploy simulation) | [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md) — delivery [projects/hard-lab/](../../projects/hard-lab/) |
| In-flight matrices while a project ships | `projects/<name>/TEST_MATRIX.md` (e.g. [adp/TEST_MATRIX.md](../../projects/adp/TEST_MATRIX.md)) |
| Suite coverage ledger (behaviors ↔ gtests ↔ skips) | `src/<layer>/<module>/tests/README.md` when non-obvious |
| Layer orientation | Optional short “Testing” blurb in `src/<layer>/README.md` linking here |

### Colocated `tests/README.md`

Do **not** require one under every peer. Add when at least one is true:

- Non-obvious split between unit vs compose vs smoke
- Explicit skips or a purpose-ID ↔ gtest matrix
- Shared harness notes (loopback fixtures, Windows SQLite teardown caveats beyond the ops convention)

**Suggested shape:**

```markdown
# Tests — <module>

Primary tier: …
Doctrine: docs/architecture/TESTING.md
Product purposes: docs/ops/TEST_STRATEGY.md (<IDs>)

## Behaviors
| Behavior | Tier | Evidence | Status | Skip |
|----------|------|----------|--------|------|
| … | unit | …_test | covered | |

## Harness notes
- …
```

Cross-cutting product journeys (calls, hop, messaging across processes) keep **pass/fail and cadence** in the ops purpose catalog. Module ledgers link **up** to those IDs; they do not redefine criteria.

### Promotion

1. Explore matrix in `projects/<name>/TEST_MATRIX.md`.
2. Land tests under `src/.../tests/`.
3. When stable, move or slim the matrix into `src/.../tests/README.md`.
4. Mark the project matrix superseded; leave rationale in project `DECISIONS.md`.
5. Keep durable purpose IDs and CI gates in ops `TEST_STRATEGY.md`.

---

## Practical default for new work

1. Put rules / codecs / stores in domain or foundation → **unit**.
2. Wire a typical path in feature with real collaborators (fakes only at true ports) → **integration**.
3. Add **smoke** only if packaging, multi-process, or env can break what lower tiers cannot see.
4. If step 2 feels huge → pause and ask whether step 1 missed a seam.

---

## Anti-patterns

- GUI as the default correctness vehicle
- Smoke that asserts unit-level codec/SM detail
- Integration sprawl because domain peers are not independent ([SRC_LAYOUT.md](SRC_LAYOUT.md))
- Line-coverage % as a gate instead of behavior accounting
- Test-only abstractions that violate layer include rules
- Duplicating pass/fail criteria in both a module ledger and the ops purpose catalog

---

## Related

| Doc | Role |
|-----|------|
| [TEST_STRATEGY.md](../ops/TEST_STRATEGY.md) | Purposes, inventory, CI, unit SQLite conventions |
| [HARD_LAB.md](../../packaging/pp-node/HARD_LAB.md) | Forced-hop / NAT / impairment scenario ladder (design) |
| [SRC_LAYOUT.md](SRC_LAYOUT.md) | Product layers and independence |
| [OWNERSHIP.md](OWNERSHIP.md) | Parent-only destroy (also applies to test fixtures) |
| [BUILD.md](../ops/BUILD.md) | `PP_BROWSER_BUILD_TESTS`, ctest |
| [UI_FUNCTIONAL_BOUNDARY.md](UI_FUNCTIONAL_BOUNDARY.md) | Prefer headless/feature paths over GUI coupling |
