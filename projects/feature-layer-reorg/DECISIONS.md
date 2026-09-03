# Feature / app reorg — decisions

Living ADRs for **method and layout**. Promote durable layout rules into [SRC_LAYOUT.md](../../docs/architecture/SRC_LAYOUT.md) and [`src/feature/README.md`](../../src/feature/README.md); mark superseded here.

---

## F001 — Sure-things-first; revisable North Star

**Date:** 2026-09-03  
**Status:** accepted

**Context:** Foundation/domain path migration is done. Feature/app still need reorg, but the ideal `feature/*` folder map is not yet proven. Premature folder renames risk large diffs without ownership clarity.

**Decision:**

1. Lock the **delivery method**: peel clear domain engines first; structural folder splits only after ownership is evidenced; each phase must leave CI green.
2. Keep a **working** North Star in [NORTH_STAR.md](NORTH_STAR.md) — revise freely.
3. Lock individual map slices only via later ADRs (F00x), then promote to SRC_LAYOUT.

**Consequences:** Agents should not open mega “final layout” PRs. Prefer CANDIDATES confidence tags. Open questions stay open until peels teach.

---

## F002 — Confidence tags for moves

**Date:** 2026-09-03  
**Status:** accepted

**Decision:** Every candidate in [CANDIDATES.md](CANDIDATES.md) carries one tag:

| Tag | Meaning | Allowed action |
|-----|---------|----------------|
| **sure** | Single peer / foundation+common; passes domain litmus | Move in f1–f3 |
| **likely** | Domain-shaped but mild coupling; short peel first | Move after noted peel |
| **blocked** | Cross-peer or orchestration | Stay feature or need `common` first |
| **structural** | Folder/CMake split, no behavior change | Only after related **sure** peels |

Do not promote **blocked** → domain to “clean the folder.”

---

## F003 — App stays composition root

**Date:** 2026-09-03  
**Status:** accepted

**Decision:** Lifetimes and cross-controller wiring remain in `src/app/`. Cleanup means **named wirers** and thin bridges — not moving orchestration into random feature headers, and not a new DI framework.

**Consequences:** `Application` may stay large in ownership terms; line-count reduction comes from splitting TUs / helpers, not from abandoning the composition root.

---

## F004 — (reserved) calls module home

**Status:** deferred until f4

Choose among: top-level `feature/calls/`, nested `feature/messaging/calls/`, or keep flat under messaging with clearer naming only. Decide with evidence from f1–f2 file counts and include edges.

---

## F005 — (reserved) shell / contacts split

**Status:** deferred until f5

Choose folder names and whether residual `feature/ui` remains.
