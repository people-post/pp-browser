# Network status chrome — current state

**As of:** 2026-08-05  
**Phase:** s0 **done** (decisions locked) — no UI implementation yet; **s1** next

## Decisions

Product answers accepted and recorded as [S003–S010](DECISIONS.md). See [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) for the resolution table.

## What ships today (pre-s1)

| Piece | Behavior |
|-------|----------|
| Desktop status bar | 24dp, desktop + expanded only; display-only |
| Left text | `Online` / `Direct off` / empty — host running only |
| Right text | Activity via `ShellHost::SetActivity` |
| Compact | Activity strip when status bar hidden |
| Me → Network | Reachability, Help the network, circuit/media toggles, Retest, UPnP, nudge |
| Relay stats | Not exposed to UI |

## Gaps for s1+

1. Cluster UI (Mesh / Reach / Help) + icons/colors/i18n  
2. Adaptive Help/Load slots for Node  
3. Hybrid popover + Retest + settings deep-link  
4. `RelayRuntimeStats` counts API  
5. Hardcoded English status bar strings  

## Next

**s1** — ambient cluster, display-only ([PHASES.md](PHASES.md)).
