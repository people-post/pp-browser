# Network status chrome — design

**Status:** Product decisions locked ([DECISIONS.md](DECISIONS.md) S003–S010, **S011**).  
**Execution order:** [PHASES.md](PHASES.md). **Today:** [CURRENT_STATE.md](CURRENT_STATE.md) — **s3 Load counts landed**.

**Stable refs:** [WINDOW_SHELL.md](../../docs/ui/WINDOW_SHELL.md), [UI_DESIGN_SYSTEM.md](../../docs/ui/UI_DESIGN_SYSTEM.md), [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md)

---

## Overview

The desktop expanded status bar is ambient chrome for **device/network posture**. It shows a **scannable status cluster** and (s2) a **click → hybrid popover** for inspection (Retest, load aggregates) with a deep-link into Me → Network for configuration.

Per-peer path (`Via relay`) stays in chat; in-call media path stays in call chrome.

**User questions the bar answers:**

| Persona | Ambient questions |
|---------|-------------------|
| **Client** | Is Brief (messaging relay) OK? Is Direct (libp2p dial-out) ready? |
| **Node / Help on** | Those · Am I helping? · Is inbound dialable? · (later) helper load |

---

## Goals

1. **Ambient truth** — Brief, Direct, helping, inbound, and (when relevant) live load counts are visible at a glance on desktop expanded layout.
2. **Scannable visuals** — Prefer icons, color, and short words over dense prose in 24dp.
3. **Progressive disclosure** — Bar = summary; click = popover; settings = configuration.
4. **One severity vocabulary** — Align with Me attention / reachability nudge; ambient color still reflects condition after nudge ack ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).
5. **Honest instrumentation** — Brief from last HTTP relay poll; Direct from libp2p running + seed dial; inbound from dial-back ([S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound)).
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
| **A** | Brief | Is HTTP messaging relay reachable? | Last `PollInbox` success/fail (`BriefRelayHealth`) |
| **B** | Direct | Is libp2p up and able to dial out? | `Libp2pHost::IsRunning` + `seed_dial_ok` / classify |
| **C** | Help | Am I offering help? | `node_enabled`, circuit/media service started |
| **D** | Inbound | Can others dial me? | `dial_back_ok` → on/off (Help on only) |
| **E** | Load | Active helper load | `RelayRuntimeStats` counts ([S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a)) |

**Adaptive visibility ([S004](DECISIONS.md#s004--adaptive-persona--slots-q2-c), [S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound)):**

| Role | Slots shown |
|------|-------------|
| Client / help off | **A Brief** + **B Direct** |
| Node / help on | A + B + **C Help** + **D Inbound**; **E Load** only when count > 0 |

Right side remains **ephemeral activity** (agent tools, PIN prep). Load never moves to the right ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)).

### Slot semantics

**A — Brief:** unknown (no poll yet) · ok · failed  

**B — Direct:** off · checking · on (seed dial OK) · error (seed dial failed / libp2p error)  

**C — Help:** off (hidden for Client) · on  

**D — Inbound (Help on):** on (Reachable) · off (Outbound only / Blocked) — binary inbound; outbound is not a separate glyph  

**E — Load:** circuit / media active counts; hide zeros  

---

## Visual language

Constrained by **24dp** height ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)), `text-xs`, design-system tokens, monochrome SVGs + `image-color`.

| Element | Role |
|---------|------|
| Brief cloud | green = ok · muted = unknown · red = failed |
| Direct link | green = dial-out ready · yellow = off · muted = checking · red = error |
| Help glyph | teal when helping |
| Inbound target | green = dialable · yellow = not dialable (Help on only) |
| Color | **Condition:** green / yellow / red. **Role:** teal = helping. |
| Layout | Brief+Direct grouped; Help+Inbound+sparse label after a vertical divider; cluster hover (popover s2) |
| Motion | **Transitional only** — no continuous pulse ([S010](DECISIONS.md#s010--chrome-polish-defaults-q8q17)) |
| Words | Sparse: `Brief offline`, `Direct off`, `Outbound only`, `Blocked` when unhealthy |

Truncation: drop numbers → Help label → keep glyphs.

---

## Click → details ([S005](DECISIONS.md#s005--click--hybrid-popover--settings-link-q3-c), [S006](DECISIONS.md#s006--detail-inspect--retest-no-capability-toggles-q4-b))

Left cluster is a hit target (s2). Opens an **anchored popover** above the bar.

### Popover content

- Brief + Direct + Reach/inbound summary + **Retest**
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
| **feature/messaging + base/p2p** | Brief poll health, host/reachability/help signals; `RelayRuntimeStats` snapshot API |
| **Me → Network (settings)** | Configuration toggles, full connection card, nudge ack |
| **Chat / Calls** | Peer-scoped and call-scoped status |
| **ShellHost** | Visibility, dirty chrome, click → popover |

Respect [UI_FUNCTIONAL_BOUNDARY.md](../../docs/architecture/UI_FUNCTIONAL_BOUNDARY.md): ports + app-owned projection; no new `::Instance()` coupling.

---

## Instrumentation

| Metric | Plan |
|--------|------|
| Brief poll ok/fail | `MeshMessagingService` atomic — now |
| Direct / inbound flags | `ReachabilitySnapshot` — now |
| Circuit / media active counts | Snapshot count — s3 ([S008](DECISIONS.md#s008--load-mvp-is-active-counts-only-q6-a)) |
| Throughput / delay | Post-MVP after counts |

---

## Coexistence

| Surface | Owns |
|---------|------|
| Status bar left | Global posture A–E |
| Status bar right | Ephemeral activity (wins over Load) |
| Compact activity strip | Unchanged when status bar hidden |
| Me attention dot | Nudge UX; ambient inbound color still reflects condition after ack |
| Chat header peer link | Per-thread path |
| Call chrome | In-call media path |

---

## i18n

Keys under `shell.statusbar.*` / settings reachability parity. Migrate hardcoded legacy strings out of ports.

---

## Dogfood note

Client: Brief cloud + Direct link. Node: + divider + Help + inbound target; under load show count pills (`circuit N` / `media N`).
