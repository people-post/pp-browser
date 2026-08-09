# Decisions — AI-centric interface

ADR-style log. Newest first within each ID series. Prefix **I** (interface / intent).

---

## I001 — Closed acts, open domains

**Date:** 2026-07-10  
**Status:** Proposed (d0)

**Decision:** The long-term intent model uses a **closed set of 10 acts** (Inquire, Discover, Produce, Operate, Navigate, Monitor, Decide, Govern, Repair, Relate) and an **open domain vocabulary** registered with tools.

**Rationale:** Acts capture planner-critical distinctions (especially Discover vs Operate vs Navigate vs Produce). Domains grow with MCP/apps without exploding the act enum. Completeness of acts beats early depth.

**Alternatives rejected:** (1) Expand `ResponseGoal` into a mega-enum mixing render and intent. (2) Unbounded act labels per feature.

---

## I002 — Agency axes: commitment + horizon

**Date:** 2026-07-10  
**Status:** Proposed (d0)

**Decision:** Alongside act×domain, plans carry **commitment** (Suggest / Confirm / Execute / Autonomous) and **horizon** (Turn / Session / Background).

**Rationale:** Same Operate intent must support chip-confirm vs auto-run vs later watchers. Encoding only in free-text `synthesis_hints` is too weak for policy and tests.

**v1 note:** Horizon may be almost always `turn`; Autonomous Monitor is post-v1 but the field exists so we do not paint into a corner.

---

## I003 — v1 ships thin paths for every act

**Date:** 2026-07-10  
**Status:** Proposed (d0)

**Decision:** Before deepening any single act, **v1 must provide a simple path for all 10 acts** (including honest stubs for Monitor/Repair that tell the user what is not supported yet).

**Rationale:** Long-lived project strategy: completeness of surface first, sophistication second. Silent gaps recreate the PeerId add-contact failure mode.

**v1 critical path:** Planner tool catalog + act field + Operate×People PeerId support (motivating bug).

---

## I004 — Render stays on ResponseGoal

**Date:** 2026-07-10  
**Status:** Proposed (d0)

**Decision:** Keep `ResponseGoal` / `RenderMode` for **reply shape and working-set routing**. Do not replace them with acts; map act×domain → suggested goal in planner rules.

**Rationale:** Working set and synthesis prompts already key off goals. Intent and render are orthogonal; conflating them caused Discover-shaped handling of Operate requests.

---

## I005 — In-chat tool permissions (parked approval)

**Date:** 2026-08-09  
**Status:** Accepted

**Decision:** Mutating agent tools (`ToolMeta.risk` write/destructive, or `mutating`) default to **Ask**. Confirmation is an **in-chat `choice` block** (Allow once / Always allow / Deny), not a modal. The runtime keeps **at most one parked approval** per agent session; out-of-sequence or stale clicks fail closed with typed errors (`approval_not_found`, `approval_superseded`, `approval_already_resolved`, `approval_expired`, `turn_busy`). Always/Never persist in profile `preferences.json` → `tool_permissions`; Me → Security can reset them. Refinement-loop mutations that still need Ask are denied with a tool error (no second park).

**Rationale:** Conversation-first UX; avoids async multi-task queues; reuses chip/payload plumbing; aligns with Confirm commitment (I002 / OI002).

**Alternatives rejected:** (1) Modal `ShowConfirm` for every mutate. (2) Unlimited parallel parked tool jobs. (3) Persisting “allow once.”

---

## Open questions

| ID | Question | Notes |
|----|----------|-------|
| OI001 | Soft-default if planner omits `act`? | Prefer hard validate + repair; soft default only if models flake in practice |
| OI002 | Confirm vs Execute allowlist for Operate | Partially answered by I005 (risk defaults + Always allow). PeerId Execute allowlist still open. |
| OI003 | PeerId-only contacts without multiaddr | Enough for address book v1; dial may need multiaddr later |
| OI004 | Where to promote stable doc | `docs/AI_CENTRIC_INTERFACE.md` vs section in AGENT_CONVERSATION |

Resolve into I-decisions before or during the relevant v1 phase.
