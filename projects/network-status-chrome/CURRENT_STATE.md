# Network status chrome — current state

**As of:** 2026-08-06  
**Phase:** **s1 polish** — Brief · Direct · Help · Inbound cluster; dogfood / s2 popover next

## Decisions

Product answers accepted and recorded as [S003–S011](DECISIONS.md). See [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

## What ships in tree (s1)

| Piece | Behavior |
|-------|----------|
| Desktop status bar | 24dp, desktop + expanded only; **display-only** |
| Left cluster | Brief cloud · Direct link · divider · Help · Inbound target · sparse label; green/yellow/red condition; teal Help |
| Brief | Last HTTP `PollInbox` ok / fail / unknown |
| Direct | libp2p running + seed dial (`On` / `Off` / `Checking` / `Error`) |
| Help | Visible when `IsHelpNetworkEnabled()` (desktop Node) |
| Inbound | Help on only: dial-back on/off |
| Labels | i18n: `shell.statusbar.brief_offline`, `direct_off`; settings parity for Outbound only / Blocked |
| Right | Activity via `ShellHost::SetActivity` (unchanged) |
| Ports | `MessagingShellPorts::statusbar_cluster` + `BuildStatusbarClusterSnapshot` |
| Tests | `statusbar_cluster_test.cpp` |

## Still open for later phases

| Gap | Phase |
|-----|-------|
| Click → hybrid popover + Retest | s2 |
| Load counts (`RelayRuntimeStats`) | s3 |
| Brief health beyond PollInbox (compat ping / Send) | polish |
| Hop “relay available” Direct enrichment | deferred ([S007](DECISIONS.md#s007--reach-uses-reachability-first-hop-relay-available-later-q5-db) / [S011](DECISIONS.md#s011--client-brief--direct-node-adds-inbound)) |
| Accessible names for icon-only states | s2/s4 |

## Next

Dogfood Client + Node desktop; then **s2** popover.
