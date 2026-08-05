# Network status chrome — design

**Status:** Product decisions locked ([DECISIONS.md](DECISIONS.md) S003–S010).  
**Execution order:** [PHASES.md](PHASES.md). **Today:** [CURRENT_STATE.md](CURRENT_STATE.md).

**Stable refs:** [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md), [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md), [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md)

---

## Overview

The desktop expanded status bar is ambient chrome for **device/network posture**. Today it shows only host readiness (`Online` / `Direct off`) plus right-side activity text. This project evolves it into a **scannable status cluster** and a **click → hybrid popover** for inspection (Retest, load aggregates) with a deep-link into Me → Network for configuration.

Per-peer path (`Via relay`) stays in chat; in-call media path stays in call chrome. The bar answers: *Is my mesh up? How reachable am I? Am I helping? How hard am I working as a helper?*

---

## Goals

1. **Ambient truth** — Mesh, reachability, helping mode, and (when relevant) live load counts are visible at a glance on desktop expanded layout.
2. **Scannable visuals** — Prefer icons, color, strength segments, and short words over dense prose in 24dp.
3. **Progressive disclosure** — Bar = summary; click = popover; settings = configuration.
4. **One severity vocabulary** — Align with Me attention / reachability nudge; ambient color still reflects condition after nudge ack ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).
5. **Honest instrumentation** — Reachability-only in s1 for slot B; counts before rates/RTT ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db), [S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a)).
6. **i18n** — All user-visible strings through locales; tone parity with settings ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).

## Non-goals

- Replacing Me → Network configuration UI ([S006](DECISIONS.md#s006--detail-inspect--retest-no-capability-toggles-q4-b)).
- Compact/mobile status bar in v1 ([S003](DECISIONS.md#s003--platforms-desktop-expanded-only-q1-a)).
- Chat peer-link or SoftMigrate hop PeerIds in the global bar.
- Helper client identities in chrome ([S009](DECISIONS.md#s009--helper-privacy-aggregates-only-q7-a)).
- `pp-node` CLI redesign ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).
- Public relay marketplace / paid relay UX.
- Growing the bar above 24dp ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).

---

## Information architecture

Left→right by stability (least → most volatile):

| Slot | Name | Question | Sources |
|------|------|----------|---------|
| **A** | Mesh | Is local libp2p up? | `Libp2pHost::IsRunning`, last error, starting |
| **B** | Reach | Inbound / outbound posture | `ReachabilitySnapshot` in s1; later + dialable hop in policy set ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db)) |
| **C** | Help | Am I offering help? | `node_enabled`, circuit/media service started |
| **D** | Load | Active helper load | `RelayRuntimeStats` counts ([S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a)) |

**Adaptive visibility ([S004](DECISIONS.md#s004--adaptive-persona--slots-q2-c)):**

| Role | Slots shown |
|------|-------------|
| Client / help off | A + B |
| Node / help on | A + B + C; D only when count > 0 |

Right side remains **ephemeral activity** (agent tools, PIN prep). Load never moves to the right ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).

### Slot semantics

**A — Mesh:** off · starting · on · error  

**B — Reach (s1):** Checking · Reachable · Outbound only · Blocked · Unknown  

**B — Reach (later):** may add Relayed / path-quality when dialable hop inventory is solid ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db)). Still a sub-signal of Reach, not a separate slot. HTTP Brief is not this signal.

**C — Help:** off (hidden for Client) · on idle · on active  

**D — Load:** circuit active count · media active count; hide zeros. Rates/delay post-MVP.

---

## Visual language

Constrained by **24dp** height ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)), `text-xs`, design-system tokens, **new** white SVG icons + `image-color`.

| Element | Role |
|---------|------|
| Mesh dot/ring | Filled = on, hollow = off, dashed/spin = starting, red = error |
| Reach segments (3) | Strength metaphor: 3 Reachable, 1 Outbound only, 0 Blocked; Checking = transitional motion |
| Help glyph | Outline/absent when off · solid teal/accent on · brief tick when becoming active |
| Load pills | Icon + integer only when > 0 |
| Color | Muted = healthy; warning = Outbound only / degraded; error = Blocked / mesh error; teal = helping |
| Motion | **Transitional only** — no continuous pulse ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)) |
| Words | Sparse: `Direct off`, `Blocked` when unhealthy; icons when healthy ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)) |

Truncation: drop numbers → Help label → keep glyphs.

---

## Click → details ([S005](DECISIONS.md#s005--click--hybrid-popover--settings-link-q3-c), [S006](DECISIONS.md#s006--detail-inspect--retest-no-capability-toggles-q4-b))

Left cluster is a hit target (s2). Opens an **anchored popover** above the bar.

### Popover content

- Reach chip + summary + **Retest**
- Help posture echo (not toggles)
- Load aggregates when helping (counts; later rates) — **no peer identities** ([S009](DECISIONS.md#s009--helper-privacy-aggregates-only-q7-a))
- Last libp2p error when present ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17))
- **Open Network settings…** → Me → Network (capability toggles, UPnP controls, listen multiaddr)

May **display** UPnP/mapped state; do not toggle capabilities here.

---

## Ownership

| Layer | Owns |
|-------|------|
| **This project** | Status cluster UX, popover, shell bindings, status icons |
| **feature/messaging + libp2p/integration** | Host/reachability/help signals; `RelayRuntimeStats` snapshot API |
| **Me → Network (settings)** | Configuration toggles, full connection card, nudge ack |
| **Chat / Calls** | Peer-scoped and call-scoped status |
| **ShellHost** | Visibility, dirty chrome, click → popover |

Respect [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md): ports + app-owned projection; no new `::Instance()` coupling.

---

## Instrumentation

| Metric | s1 / s3 plan |
|--------|----------------|
| Mesh / reach / help flags | Existing APIs — s1 |
| Circuit active bridges/clients | Snapshot count — s3 ([S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a)) |
| Media sessions / participants | Snapshot count — s3 |
| Throughput / delay | Post-MVP after counts |

---

## Coexistence

| Surface | Owns |
|---------|------|
| Status bar left | Global posture A–D |
| Status bar right | Ephemeral activity (wins over Load) |
| Compact activity strip | Unchanged when status bar hidden |
| Me attention dot | Nudge UX; ambient Reach color still reflects condition after ack |
| Chat header peer link | Per-thread path |
| Call chrome | In-call media path |

---

## i18n

Keys under `shell.statusbar.*` / `shell.network_status.*` (names TBD). Reuse `settings.network.reachability.*` where the same words appear. Migrate hardcoded `Online` / `Direct off` out of ports.

---

## Dogfood gate ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17))

Node desktop: Reachable + Helping idle visible; click → popover with Retest; under load show count pills. One LAN helper + one client is enough for s1/s2.
