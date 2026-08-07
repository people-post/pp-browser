# Network status chrome — current state

**As of:** 2026-08-07  
**Phase:** **s3 landed** — Load counts; **s4** polish next

## Decisions

Product answers accepted and recorded as [S003–S011](DECISIONS.md). See [OPEN_QUESTIONS.md](OPEN_QUESTIONS.md).

## What ships in tree (s1–s3)

| Piece | Behavior |
|-------|----------|
| Desktop status bar | 24dp, desktop + expanded only |
| Left cluster | Brief · Direct · divider · Help · Inbound · Load pills · sparse label |
| Brief / Direct / Help / Inbound | As s1 |
| Load (s3) | Help on only; `circuit N` / `media N` when count > 0 (aggregates from `RelayRuntimeStats`) |
| Popover | s2 inspect + Retest + deep-link; **Helper load** rows when helping under load |
| Ports | `MessagingShellPorts` + `CircuitRelayService::RuntimeStats` / `MediaRelayService::RuntimeStats` |
| Tests | `statusbar_cluster_test.cpp` (cluster + popover + load) |

## Still open for later phases

| Gap | Phase |
|-----|-------|
| Brief health beyond PollInbox | polish |
| Hop “relay available” Direct enrichment | deferred |
| Truncation / EN+zh-Hans width budget | s4 |
| Transitional motion | s4 |
| Throughput / delay | post-MVP |

## Next

**s4** polish (truncation / width budget, transitional motion, promote normative bits to `docs/ui/`).
