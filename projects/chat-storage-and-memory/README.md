# Chat storage, memory, and messaging channels

**Status:** Planning — v2 not started (as of 2026-06-29)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [e2e-message-crypto](../e2e-message-crypto/) (symmetric E2E body crypto; depends on v2b + v6 for wire-up)  
**Recent:** D079–D086 (contact vs communicating identity; public ephemeral inbound; peer signing key lookup E016; relay identity format; retired PSK ledger E018; PSK in `profile.db` D084; passive epoch adopt D085; rich OOB bundle D086/E020) — see [DECISIONS.md](DECISIONS.md)

## One-line goal

One durable conversation model for AI and P2P chat: SQLite per thread, `profile.db` catalog/outbox/`chat_targets` (identity-keyed `ChatTargetKey`, D056/D079), local `thread_id` only, `display_order` paging (D054), ChatPayload v1, channel badges, E2E seq sync with **peer-first backfill** (D058–D060) and **user-initiated sync** (D059) — richer types, scroll backfill, shared `@ai`, and transport badges phased after v1.

**Related:** [platform-safety-limits](../platform-safety-limits/) (LLM HTTP, profile stores — outside chat wire).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | **Complete system specification** — all behavior with `[v1]` / `[post-v1]` maturity tags |
| [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) | **Normative JSON wire shapes** — envelope, ChatPayload, history fetch (D072) |
| [PHASES.md](PHASES.md) | **Implementation order** — checklists, exit criteria, traceability (no duplicate specs) |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style rationale) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |

## v1 implementation order

See [PHASES.md](PHASES.md) for full checklists. Summary:

| Phase | Focus |
|-------|--------|
| v2a | SqliteThreadStore, reconciliation, `GetMessagesPage`, `ChatTargetKey` routing, clear history + **confirmation dialog** (D057) |
| v2b | Public vs E2E split, channel badge sidebar |
| v3 | AI memory + compaction |
| v4 | ChatPayload text/system, transport column |
| v6 | E2E seq, tail sync, gap repair, **user sync**, integrity UX |

`[post-v1]` work (post-v4, post-v6b–e) is specified inline in DESIGN and scheduled in PHASES.

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| — | Baseline (JSON `JsonThreadStore`, router, sliding context) | Done (pre-project) |
| v2a | **SqliteThreadStore** + unified transcript (D057: **v2a-core** then **v2a-p2p**) | Not started |
| v2b | Public vs E2E channel split + badges | Not started |
| v3 | Durable AI memory + clear/forget UX | Not started |
| v4 | ChatPayload text/system + transport column | Not started |
| v6 | E2E sender seq + tail/gap/**user** sync + D067/D068 integrity | Not started |

## Open questions

**None in this project** — O001–O005 resolved as D023–D027.

**Cross-project (e2e-message-crypto):** peer signing keys resolved (E016 / D081); PSK UX and wire-up after chat-storage **v2b + v6** — see [PHASES.md](PHASES.md) § Cross-project.
