# Network status chrome — design

**Status:** Draft pending [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).  
**Execution order:** [PHASES.md](PHASES.md). **Today:** [CURRENT_STATE.md](CURRENT_STATE.md). **Rationale:** [DECISIONS.md](DECISIONS.md).

**Stable refs:** [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md), [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md), [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md)

---

## Overview

The desktop expanded status bar is ambient chrome for **device/network posture**. Today it shows only host readiness (`Online` / `Direct off`) plus right-side activity text. This project evolves it into a **scannable status cluster** and a **click → details** surface for inspection and actions (retest, open settings, inspect relay load).

Per-peer path (`Via relay`) stays in chat; in-call media path stays in call chrome. The bar answers: *Is my mesh up? How reachable am I? Am I helping? How hard am I working as a helper?*

---

## Goals

1. **Ambient truth** — Mesh, reach/path quality, helping mode, and (when relevant) live load are visible at a glance on desktop expanded layout.
2. **Scannable visuals** — Prefer icons, color, strength segments, and short words over dense prose in 24dp.
3. **Progressive disclosure** — Bar = summary; click = detail (stats, actions, links into Me → Network).
4. **One severity vocabulary** — Align with Me attention / reachability nudge (no conflicting “healthy” vs “problem” stories).
5. **Honest instrumentation** — Do not invent rates/RTT in UI before services publish them.
6. **i18n** — All user-visible strings go through locales (status bar today is hardcoded English).

## Non-goals

- Replacing Me → Network configuration UI.
- Showing chat peer-link or SoftMigrate hop PeerIds in the global bar.
- Compact/mobile status bar in v1 (visibility rules may stay desktop+expanded unless OPEN_QUESTIONS change this).
- Public relay marketplace / paid relay UX.
- Turning the 24dp bar into a full metrics dashboard.

---

## Information architecture

Left→right by stability (least → most volatile):

| Slot | Name | Question | Sources (candidate) |
|------|------|----------|---------------------|
| **A** | Mesh | Is local libp2p up? | `Libp2pHost::IsRunning`, last error, starting |
| **B** | Reach | Path quality / inbound vs relayed | `ReachabilitySnapshot` + consumer-relay availability signals |
| **C** | Help | Am I offering help? | `node_enabled`, circuit/media service started |
| **D** | Load | How hard am I working as helper? | New `RelayRuntimeStats` (counts → rates → delay) |

Right side remains **ephemeral activity** (agent tools, PIN prep; optionally call media activity later). Load (D) must not fight activity for the same pixels — see open questions.

### Slot semantics (draft)

**A — Mesh:** off · starting · on · error  

**B — Reach (fused path quality):**  
Direct / Reachable · Outbound · Relayed (outbound/blocked but usable relay path) · Blocked · Checking  

“Relay server availability” is a **sub-signal of Reach**, not a fifth peer glyph.

**C — Help:** off · on idle · on active  

**D — Load:** shown only when Help is on and load > 0 (default). Circuit clients, media sessions/participants; rates/delay when instrumented. Hide zeros.

---

## Visual language (draft)

Constrained by 24dp height, `text-xs`, design-system tokens, white SVG icons + `image-color`.

| Element | Role |
|---------|------|
| Mesh dot/ring | Filled = on, hollow = off, dashed/spin = starting, red = error |
| Reach segments (3) | Strength metaphor (reuse call-meter visual idea): 3 Direct, 2 Relayed/path, 1 Outbound, 0 Blocked |
| Help glyph | Outline off · solid teal/accent on · pulse when actively relaying |
| Load pills | Icon + small integer (and optional rate) only when > 0 |
| Color | Muted = healthy; `semantic-warning-*` = degraded; `semantic-error` = blocked/error; teal = helping |
| Motion | One subtle pulse for Checking / active help — not decorative noise |
| Words | Spare: `Direct off`, `Blocked`, maybe `Helping` — prefer glyphs otherwise |

Density: always A+B; C when Node/help UI exists; D only when active (or user pins detail mode later). Truncation drops numbers → Help label → keep glyphs.

---

## Click → details (draft)

Bar becomes a hit target (whole cluster or left segment). Detail surface options (unresolved — see OPEN_QUESTIONS):

| Option | Pattern | Pros |
|--------|---------|------|
| **Popover / anchored panel** | New light chrome above bar | Fast inspect without leaving context |
| **Bottom/side sheet** | Existing sheet primitives | Familiar on compact if we expand later |
| **Deep-link only** | Navigate to Me → Network | Least new UI; weaker live stats home |
| **Hybrid** | Popover summary + “Open Network settings…” | Likely best of both |

### Detail content (candidate inventory)

- Reach chip + summary + Retest (reuse settings ports)
- Help posture: node on/off echo; circuit/media capability state
- Live load tables: circuit active clients; media sessions/participants; up/down rate; delay; budget headroom
- Last libp2p error (when present)
- Link/button → Me → Network for toggles (UPnP, capabilities, listen multiaddr)

Actions in the detail surface (vs settings-only) are an open question.

---

## Ownership

| Layer | Owns |
|-------|------|
| **This project** | Status cluster UX, detail surface, shell bindings, visual tokens/icons for status chrome |
| **feature/messaging + libp2p/integration** | Host/reachability/help signals; new relay runtime stats snapshot API |
| **Me → Network (settings)** | Configuration toggles, full connection card, nudge ack |
| **Chat / Calls** | Peer-scoped and call-scoped status (stay out of global bar except optional right-side activity) |
| **ShellHost** | Visibility, dirty chrome, click routing into detail primitive |

Respect [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md): no new `::Instance()` coupling; prefer ports + app-owned projection (extend `MessagingShellPorts` / add a small status presenter bridge).

---

## Instrumentation (needed for Load)

| Metric | Today | Needed |
|--------|-------|--------|
| Circuit active bridges/clients | Private `active_bridges` | Snapshot count API |
| Media sessions / participants | Private maps | Snapshot count API |
| Media byte totals | Private cumulatives | Publish + windowed rate |
| Circuit byte/rate | None | Counters on `StreamBridge` |
| Delay / RTT | None | Session-level measurement |

UI must degrade gracefully: show counts before rates; omit delay until real.

---

## Coexistence rules (draft)

| Surface | Owns |
|---------|------|
| Status bar left | Global device posture A–D |
| Status bar right | Ephemeral activity (agent / PIN; maybe call) |
| Compact activity strip | Unchanged when status bar hidden |
| Me attention dot | Same severity as Reach problem states |
| Chat header peer link | Per-thread path |
| Call chrome | In-call media path |

---

## i18n

New keys under something like `shell.statusbar.*` and `shell.network_status.*` (names TBD). Reuse `settings.network.reachability.*` where the same words appear. No hardcoded English in ports (today’s `Online` / `Direct off` migrate here).

---

## Open work

Resolve [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) → record ADRs in [DECISIONS.md](DECISIONS.md) → implement per [PHASES.md](PHASES.md).
