# Call path resilience

**Status:** Planned — design agreed 2026-09-24; no phase landed  
**Owner:** Hongwei + agents  
**Stable refs:** [CALLS.md](../../docs/architecture/CALLS.md), [MESH.md](../../docs/architecture/MESH.md), [NETWORKING.md](../../docs/architecture/NETWORKING.md)  
**Related:** [p2p-av-calls](../p2p-av-calls/) (planners, V038/V049), [media-hop-reachability](../media-hop-reachability/) (punch / circuit, L3.25c), [adp](../adp/) (A003 path migrate, A024 nested carrier), [network-status-chrome](../network-status-chrome/) (path display), [hard-lab](../hard-lab/)

## One-line goal

A live call **survives path changes** — relay ↔ punched ↔ direct, NAT rebinding, network change — by moving media **make-before-break** under a path policy chosen from **how stable each endpoint's network is** (stationary vs mobile).

## Why now

Dogfood 2026-09-24 (answerer at home, caller on an outside network):

1. SIGSEGV when the relay carrier reset mid nested handshake — **fixed** in pp-cpp-amp v2.1.9 (deferred drop in `FinishNestedCarrier`).
2. After the fix: call connects over the relay circuit, punch succeeds 3.5 s later, planner logs `planner=Direct keep` (media stays on the circuit), **received audio stops exactly 10.0 s after the punch**, and ~7 s later the leg tears down with `amp call-media: peer link lost`.

Nothing moves a live 1:1 call between links today, no one owns the relay once a direct path exists, Amp links fail silently, and the app never reacts to network changes. Details: [DESIGN.md § Current state](DESIGN.md#current-state-2026-09-24).

## Release scope (v1)

| In | Out |
|----|-----|
| Amp link events (connected / dropped + reason / path change / transport kind) and app logging | Amp-internal logging framework |
| Amp link hygiene bugs found in the survey | New L4 protocol ids (frozen — A028/N030) |
| Call links kept alive (hot) and relay kept as warm standby for the call | Multipath media (sending on two paths at once) beyond the migration overlap |
| Make-before-break migration of 1:1 call media between links, explicit two-sided relay release | N≥3 SFU SoftMigrate changes (owned by p2p-av-calls) |
| Media-liveness failover + "Reconnecting…" instead of teardown | ICE / TURN |
| Network-change detection on all platforms; re-anchor on change | Motion / location sensors |
| Automatic mobility classification, `caps.mobility`, pair path policy | Asking the user to pick a mode (optional preference only) |

## Documents

| File | Tier | Purpose |
|------|------|---------|
| [DESIGN.md](DESIGN.md) | Spec | Model, mechanisms, policy table, wire, failure UX |
| [DECISIONS.md](DECISIONS.md) | ADRs | K001– |
| [PHASES.md](PHASES.md) | Order | k0–k7 checklists |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Status | Only place for phase status |

## Dependencies

- **pp-cpp-amp** — k0 events, k1 hygiene; releases tagged and pinned in `cmake/PpCppAmp.cmake`.
- **p2p-av-calls** — Direct planner events and `CallMediaLegCoordinator` bundle model (k3/k4 change them).
- **media-hop-reachability** — defines "request circuit close when safe" (L3.25c) — this project supplies "safe" (K002).
