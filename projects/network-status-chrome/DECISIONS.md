# Network status chrome — decisions

ADRs for this project. Product answers resolved 2026-08-05 (see [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md)).  
Normative UX summary: [DESIGN.md](DESIGN.md).

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

## S003 — Platforms: desktop expanded only (Q1 A)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** v1 status cluster visibility stays **desktop + expanded layout** only (same gate as today’s status bar). Compact desktop and mobile/tablet are out of scope until a later ADR.

**Rationale:** Compact already has the activity strip and Me sheet; inventing a second placement fights chrome density.

**Consequences:** No change to `RefreshStatusbarVisibility` platform rules in s1 beyond cluster content inside the existing bar.

---

## S004 — Adaptive persona / slots (Q2 C)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Slot visibility is **adaptive**:

| Role | Slots |
|------|-------|
| Client / help off | **A Mesh** + **B Reach** |
| Node / help on | A + B + **C Help** + **D Load** (Load only when count > 0) |

**Rationale:** Everyday users stay calm; helpers get contribution + load without forcing Client UI to show “Helping off.”

**Consequences:** Ports must expose role/help posture, not only host running.

---

## S005 — Click → hybrid popover + settings link (Q3 C)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Clicking the left cluster opens a **lightweight anchored popover** above the bar, with an explicit control **“Open Network settings…”** that deep-links to Me → Network. Not deep-link-only; not a full overlay twin of settings.

**Rationale:** Fast inspect without leaving context; settings remain configuration home.

**Consequences:** New shell presentation primitive (or reuse closest pane/sheet pattern) in **s2**; s1 may stay display-only until popover lands.

---

## S006 — Detail inspect + Retest; no capability toggles (Q4 B)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Popover is **inspect + limited actions**: show reach summary, help/load posture, last error; allow **Retest**. May **display** UPnP/mapped state. Do **not** toggle Help / circuit / media / node from the popover — those stay in Me → Network.

**Rationale:** Avoid two sources of truth for capabilities; Retest is already a safe settings port.

**Consequences:** Reuse settings reachability/retest ports; capability toggles only via deep-link.

---

## S007 — Reach uses reachability first; hop “relay available” later (Q5 D→B)

**Date:** 2026-08-05  
**Status:** Partially superseded by [S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound) for ambient Client slots / HTTP Brief exclusion. Hop enrichment clause still stands.

**Decision:**

1. **s1 (historical):** Slot B = `ReachabilitySnapshot` only (`Checking` / `Reachable` / `OutboundOnly` / `Blocked` / `Unknown`). No “Relayed because hop X exists” glyph until hop inventory is trustworthy.
2. **Later:** Extend Direct/inbound with consumer **dialable circuit/media hop in policy set** (option B) as a path-quality upgrade — still not a separate Brief substitute.

**Rationale:** Honest ambient UI; don’t invent relay availability.

**Consequences:** DESIGN “Relayed” state is **reserved**. Ambient HTTP Brief is now slot A per S011 (this ADR’s “HTTP Brief is not this signal” applied only to the old Reach slot).

## S008 — Load MVP is active counts only (Q6 A)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** First Load ship = **active counts** only (circuit bridges/clients, media sessions/participants). Throughput and delay/RTT are **post-MVP** (detail first, bar only if calm).

**Rationale:** Counts are derivable from existing private maps; rates/RTT need new measurement.

**Consequences:** s3 delivers count snapshot API before rate/RTT work. Bar hides Load when all counts are 0.

---

## S009 — Helper privacy: aggregates only (Q7 A)

**Date:** 2026-08-05  
**Status:** Accepted  

**Decision:** Bar and popover show **aggregates only** (counts; later rates). No contact names or PeerIds for active relay clients in v1.

**Rationale:** Safer for screenshots/screen-share; identities can be an Advanced later if needed.

**Consequences:** Detail tables are numeric/summary rows, not peer rosters.

---

## S010 — Chrome polish defaults (Q8–Q17)

**Date:** 2026-08-05  
**Status:** Accepted  

| Topic | Decision |
|-------|----------|
| **Q8 Height** | Keep **24dp** single line; truncate rather than grow |
| **Q9 Icons** | Ship **new** monochrome SVGs (mesh, reach-bars, help, circuit, media as needed); tint via `image-color` |
| **Q10 Activity vs Load** | **Activity wins the right side**; Load stays under Help on the **left** |
| **Q11 Severity after ack** | Ambient Reach **still recolors** when OutboundOnly/Blocked even if Me nudge was acked (ack dismisses nudge, not condition) |
| **Q12 Words vs icons** | **Icons when healthy**; short word only for bad/off (`Blocked`, `Direct off`) |
| **Q13 pp-node** | **GUI project only**; CLI `--status` JSON stays separate (may align later) |
| **Q14 Tone** | **Parity with settings** strings (`Outbound only`, `Reachable`, `Helping`, …) |
| **Q15 Motion** | **Transitional only** (Checking → result; brief Help-active tick) — no continuous pulse |
| **Q16 Errors** | Last libp2p error in **popover + Me → Network**; bar uses error glyph/color only |
| **Q17 Dogfood** | Node desktop: Reachable + Helping idle visible; click → hybrid with Retest; under load show count pills (`circuit N` / `media N`). One LAN helper + one client is enough for s1/s2 |

**Consequences:** Design-system icon inventory grows; a11y names for icon-only states land with s2/s4; no `pp-node` work in this project’s phases.

---

## S011 — Client Brief + Direct; Node adds inbound

**Date:** 2026-08-06  
**Status:** Accepted  
**Supersedes:** Slot IA in [S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db) (HTTP Brief was excluded from ambient; mesh+NAT strength bars as Client primary). [S004](DECISIONS.md#s004--adaptive-persona--slots-q2-c) slot letters remapped.

**Decision:**

1. **Group 1 (all):** **Brief** (HTTP relay / `brief.global` poll health) + **Direct** (libp2p running and seed dial OK).
2. **Group 2 (Help on):** divider · **Help** · **Inbound** binary (dial-back on/off). Outbound is not a separate glyph; Outbound-only shows Direct on + Inbound off + sparse label.
3. Brief health = last `PollInbox` ok/fail (`Unknown` until first poll). Not a substitute for Me → Network detail.
4. Remove 3-bar reach strength meter from the bar.

**Rationale:** Client happiness is Brief messaging + P2P dial-out when needed. Inbound dialability is a Node/helper concern. Matches how the product actually works (HTTP relay + optional libp2p).

**Consequences:** New icons `status-brief.svg`, `status-direct.svg`, `status-inbound.svg`; `MeshDeliveryOrchestrator` tracks brief poll health; shell bindings renamed from mesh/reach to brief/direct/inbound. Hop “relay available” upgrade (old S007 later clause) remains deferred as Direct/inbound enrichment, not a Brief substitute.
