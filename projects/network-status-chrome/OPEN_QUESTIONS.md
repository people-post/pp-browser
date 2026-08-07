# Open questions — network status chrome

**Status:** Blocking + important sets **resolved 2026-08-05** (product accepted agent recommendations).  
**ADRs:** [DECISIONS.md](DECISIONS.md) S003–S011.  
**Normative summary:** [DESIGN.md](DESIGN.md).

New questions should be added below **Still open**; do not reopen resolved rows without a superseding ADR.

---

## Resolved — blocking

| Q | Resolution | ADR |
|---|------------|-----|
| Q1 Platforms | **A** — desktop expanded only | [S003](DECISIONS.md#s003--platforms-desktop-expanded-only-q1-a) |
| Q2 Persona | **C** then **S011** — Client Brief+Direct / Node adds Help+Inbound (+ Load later) | [S004](DECISIONS.md#s004--adaptive-persona--slots-q2-c), [S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound) |
| Q3 Click | **C** — hybrid popover + Open Network settings… | [S005](DECISIONS.md#s005--click--hybrid-popover--settings-link-q3-c) |
| Q4 Detail vs Me | **B** — inspect + Retest; no capability toggles | [S006](DECISIONS.md#s006--detail-inspect--retest-no-capability-toggles-q4-b) |
| Q5 Relay available | **D→B** then **S011** — Brief is its own slot; Direct/inbound from reachability; hop later | [S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db), [S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound) |
| Q6 Load MVP | **A** — active counts only | [S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a) |
| Q7 Privacy | **A** — aggregates only | [S009](DECISIONS.md#s009--helper-privacy-aggregates-only-q7-a) |

---

## Resolved — important

| Q | Resolution | ADR |
|---|------------|-----|
| Q8 Height | Keep 24dp | [S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17) |
| Q9 Icons | New monochrome SVGs | S010 |
| Q10 Activity vs Load | Activity right; Load left under Help | S010 |
| Q11 Severity after ack | Still recolor ambient Reach | S010 |
| Q12 Words vs icons | Icons healthy; words for bad/off | S010 |
| Q13 pp-node | GUI only | S010 |
| Q14 Tone | Settings string parity | S010 |
| Q15 Motion | Transitional only | S010 |
| Q16 Errors | Popover + Me; bar glyph/color only | S010 |
| Q17 Dogfood | Node helper + counts under load | S010 |

---

## Deferred (explicit)

- Compact/mobile status cluster placement  
- Public/paid relay status  
- Per-peer path in the global bar  
- Historical graphs (rates over time)  
- User-configurable status bar modules  
- Contact names / PeerIds in helper detail (supersedes would need new ADR vs S009)  
- Throughput / delay in bar (post-MVP vs S008)  

---

## Still open

_None for s1. Implementation discoveries go here or as new ADRs._
