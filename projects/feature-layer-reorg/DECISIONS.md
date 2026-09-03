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

---

## F006 — Sure peels use existing domain peers (no new peers)

**Date:** 2026-09-03  
**Status:** accepted

**Context:** f1–f3 will move stores/helpers out of `feature/`. Question: add `domain/calls`, `domain/psk`, etc., or reshape peer trees first?

**Decision:**

1. **Do not add top-level domain peers** for sure peels. Keep the peer set: `people`, `messaging`, `media`, `mesh`, `net`, `ui`, `ai`.
2. **Land files in the peer that already owns the sibling types** (flat unless that peer already nests).
3. **Do not invent `domain/calls`.** Call session types/stores already live in `domain/messaging`; capture/playback stays in `domain/media`. A new calls peer would either link messaging (banned) or orphan `CallSessionStore`.
4. **Optional internal subfolders** under `domain/messaging/` (e.g. `call/`, `psk/`, `thread/`) are a **later** readability pass — not a prerequisite for f1. Prefer flat drops next to existing `Call*`, `Psk*`, `Sqlite*` files first.
5. **`CallMediaKeyStore` → `domain/messaging`**, not `domain/media` — it is vault/SQLite next to `CallSessionStore`; `domain/media` is engine/codec/OS capture.

**Homes for sure candidates:**

| Candidate | Home |
|-----------|------|
| `SqlitePskSessionStore`, PSK/epoch coordinators | `domain/messaging/` (flat) |
| `CallMediaKeyStore` | `domain/messaging/` (flat) |
| `ContactReachability`, `PeerBriefRoute`, `ProfileIconFetchUtil` | `domain/people/` (flat) |
| `MobileEphemeralListenGate` | `domain/mesh/reachability/` (nested — peer already nests) |
| `PeoplePickerLogic`, `CallConflictCopy` | `domain/ui/` (flat) |

**Consequences:** f1–f3 are path/CMake moves into known peers. Messaging stays a large flat folder for now; revisit internal banding only if navigation pain remains after peels.
