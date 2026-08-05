# Open questions — network status chrome

**Purpose:** Clarifications that reshape implementation (layout, APIs, click target, metrics).  
**Process:** Answer here or in chat → record ADR in [DECISIONS.md](DECISIONS.md) → check the box.  
**Do not implement** status-cluster / detail UI until the **blocking** set below is resolved (or explicitly deferred with an ADR).

---

## Blocking (decide before s1 code)

### Q1 — Audience & platforms

Who is the status cluster for in v1?

| Option | Meaning |
|--------|---------|
| **A** | Desktop expanded only (today’s visibility) |
| **B** | Desktop expanded + compact desktop (taller or overflow menu) |
| **C** | Also mobile/tablet (needs new compact placement — conflict with activity strip) |

**Impacts:** RML visibility, hit targets, whether detail is popover vs sheet.

### Q2 — Primary persona

Is the bar mainly for:

| Option | Emphasis |
|--------|----------|
| **A** | Everyday users (“am I online / can people reach me?”) — fewer glyphs, more plain language |
| **B** | Node helpers / power users (“am I helping / load”) — Help + Load first-class |
| **C** | Both, adaptive — Client sees A+B; Node adds C+D |

**Impacts:** Which slots are mandatory; copy tone; whether Client ever sees Help/Load.

### Q3 — Click target & detail primitive

When the user clicks the left cluster, what opens?

| Option | Primitive |
|--------|-----------|
| **A** | Anchored popover above the status bar (new) |
| **B** | Existing Me account/settings path → Network section (deep-link only) |
| **C** | Hybrid: lightweight popover + “Open Network settings…” |
| **D** | Dedicated overlay layer / sheet |

**Impacts:** Shell APIs, presentation taxonomy, how much live stats UI we build vs reuse settings.

### Q4 — Relationship to Me → Network

Should the detail surface:

| Option | Rule |
|--------|------|
| **A** | **Inspect-only** + link to settings for all toggles |
| **B** | **Inspect + a few actions** (Retest, maybe UPnP) but not capability toggles |
| **C** | **Full twin** of Connection / Help controls (risk of two sources of truth) |

**Impacts:** Ports, duplication, nudge-ack ownership.

### Q5 — “Relay server availability” definition

What should Reach’s relay sub-signal mean?

| Option | Definition |
|--------|------------|
| **A** | Org/HTTP Brief relay reachable (durability inbox) |
| **B** | At least one dialable circuit and/or media hop in policy set |
| **C** | Separate consumer signal: “I can use relays” vs provider “I am a relay” |
| **D** | Defer relay-availability glyph; Reach = `ReachabilitySnapshot` only until hop inventory is solid |

**Impacts:** New probes vs reuse peer/hop APIs; honesty of “Relayed” state.

### Q6 — Load metrics MVP

Minimum Load (slot D) for first ship:

| Option | Includes |
|--------|----------|
| **A** | Active counts only (circuit bridges, media sessions/participants) |
| **B** | A + aggregate throughput (needs rate windows) |
| **C** | B + delay/RTT |
| **D** | No Load in bar until counts API exists; detail can say “stats coming” |

**Impacts:** Instrumentation phase order; whether s2 UI waits on s3 stats.

### Q7 — Privacy / disclosure when helping

When Help is on, may the bar/detail show:

| Option | Disclosure |
|--------|------------|
| **A** | Aggregates only (counts, rates) — no peer identities |
| **B** | Aggregates + contact display names for active clients (if known) |
| **C** | Aggregates + PeerIds (power-user / debug) |

**Impacts:** Detail table columns; contact resolution; screenshot sensitivity.

---

## Important (decide before click-detail / polish)

### Q8 — Bar height & layout budget

Keep **24dp** single line, or allow a slightly taller bar / two-row cluster when Load is active?

**Impacts:** `ShellConfig::statusbar_height_dp`, content inset, truncation strategy.

### Q9 — Iconography

Ship new SVGs (mesh / reach-bars / help / circuit / media) vs reuse/tint existing (`sync`, `share`, etc.) for v1?

**Impacts:** Asset work, recognizability, design-system inventory.

### Q10 — Right-side activity vs Load

If agent is `Thinking…` and media relay is hot, who wins the right side? Options: activity always wins · Load moves under Help on the left · combine with priority rules · temporary dual-line.

### Q11 — Severity & Me attention

Should Blocked / OutboundOnly **recolor** the bar cluster even when the Me nudge was already acked?

**Impacts:** Trust in ambient color vs “I dismissed the nudge.”

### Q12 — Words vs icons default

Prefer icon-only healthy states (tooltip/accessible name later) or always a short text label beside Mesh/Reach?

**Impacts:** Width, i18n, a11y story (tooltips not free in RmlUi).

### Q13 — pp-node / headless

Does `pp-node` need a parallel text/JSON status story aligned with the same slot model, or is GUI-only in scope?

### Q14 — Localization & tone

Short technical labels (`Outbound only`, `Helping`) vs friendlier (`Hard to reach`, `Sharing connection`)? Keep parity with existing settings strings?

### Q15 — Animation budget

Allow continuous pulse for active Help/Load, or only transitional motion (Checking → result) for perf/battery on integrated GPUs?

### Q16 — Failure copy

When libp2p fails to start, show last error snippet in the bar, only in detail, or only in Me → Network?

### Q17 — Testing / dogfood gate

What is “good enough” to ship s1 (e.g. Node desktop: Reachable + Helping idle visible; click opens hybrid detail with Retest)? Any must-dogfood topology (two NATs, helper under load)?

---

## Deferred unless you pull them in

- Compact/mobile status cluster placement  
- Public/paid relay status  
- Per-peer path in the global bar  
- Historical graphs (rates over time)  
- User-configurable status bar modules  

---

## Suggested answer format

For each Qi, reply with option letter + any caveats. Example:

> Q1 A · Q2 C · Q3 C · Q4 B · Q5 D then B later · Q6 A · Q7 A · Q8 keep 24dp · …

Partial answers are fine; mark “defer” explicitly so ADRs can freeze scope.
