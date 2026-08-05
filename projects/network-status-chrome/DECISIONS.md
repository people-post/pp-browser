# Network status chrome — decisions

ADRs for this project. **Open clarifications:** [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).  
Draft design intent (not yet ADR-locked) lives in [DESIGN.md](DESIGN.md).

---

## S001 — Separate project for status chrome + detail

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Track status bar evolution and click→details as project `network-status-chrome`, not as a subsection of p2p-mesh or settings-only docs.

**Rationale:** Cross-cuts shell UX, messaging ports, reachability, and relay stats. Mesh policy stays in [p2p-mesh](../p2p-mesh/); this project owns ambient presentation and disclosure.

**Consequences:** Promote normative UI rules into `docs/ui/WINDOW_SHELL.md` (+ design system) when phases ship; keep checklists here.

---

## S002 — Broader scope: bar + post-click detail

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Project scope includes the full disclosure path (cluster → detail surface), not only left-side labels. Implementation still phased; click may ship after ambient cluster.

**Rationale:** Product intent is watch + inspect; designing bar slots without the detail sink invites overcrowding the 24dp bar.

---

## S003+ — Pending product answers

The following will become ADRs when [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) resolves:

| Future ADR | Question |
|------------|----------|
| S003 | Platforms / visibility (Q1) |
| S004 | Persona / adaptive slots (Q2) |
| S005 | Click primitive (Q3) |
| S006 | Detail vs Me → Network (Q4) |
| S007 | Relay-availability definition (Q5) |
| S008 | Load MVP metrics (Q6) |
| S009 | Helper privacy disclosure (Q7) |

No implementation ADRs until blocking questions are answered.
