# Network status chrome — current state

**As of:** 2026-08-05  
**Phase:** **s1 in progress** — ambient cluster landed in code; dogfood / polish remain

## Decisions

Product answers accepted and recorded as [S003–S010](DECISIONS.md). See [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

## What ships in tree (s1)

| Piece | Behavior |
|-------|----------|
| Desktop status bar | 24dp, desktop + expanded only; **display-only** |
| Left cluster | Mesh dot · Reach 3-bars · Help icon (Node) · sparse label |
| Mesh | on / off / error from host running + last libp2p error |
| Reach | `ReachabilitySnapshot` only ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db)) |
| Help | Visible when `IsHelpNetworkEnabled()` (desktop Node) |
| Labels | i18n: `shell.statusbar.direct_off`; settings parity for Outbound only / Blocked |
| Right | Activity via `ShellHost::SetActivity` (unchanged) |
| Ports | `MessagingShellPorts::statusbar_cluster` + `BuildStatusbarClusterSnapshot` |
| Tests | `statusbar_cluster_test.cpp` |

## Still open for later phases

| Gap | Phase |
|-----|-------|
| Click → hybrid popover + Retest | s2 |
| Load counts (`RelayRuntimeStats`) | s3 |
| Hop “relay available” Reach upgrade | post-s1 ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db)) |
| Accessible names for icon-only states | s2/s4 |
| Static HTML mock (optional) | s0 leftover |

## Next

Dogfood s1 on Node desktop; then **s2** popover.
