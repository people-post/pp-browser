# Network status chrome — current state

**As of:** 2026-08-05  
**Phase:** s0 (docs only — no UI implementation yet)

## What ships today

| Piece | Behavior |
|-------|----------|
| Desktop status bar | 24dp, desktop + expanded only; display-only |
| Left text | `Online` / `Direct off` / empty — from `MessagingShellPorts::statusbar_connection` (`Libp2pHost::IsRunning` only) |
| Right text | Activity via `ShellHost::SetActivity` (agent tools, PIN “Preparing…”) |
| Compact | Activity strip when status bar hidden; no status cluster |
| Me → Network | Reachability chip/summary, Help the network, circuit/media toggles, Retest, UPnP, nudge |
| Chat header | Per-peer link labels (`Direct`, `Via relay`, …) |
| Call chrome | In-call media path copy |
| Relay stats | **Not** exposed — circuit/media keep private session maps / bridges |

## Gaps this project fills

1. Reachability (and path quality) not reflected in the status bar  
2. Helping / relay load invisible ambiently  
3. No click → inspect surface from the bar  
4. Hardcoded English status bar strings  
5. No public relay runtime stats API for UI  

## Next

Answer [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md) (blocking Q1–Q7), then ADRs → **s1**.
