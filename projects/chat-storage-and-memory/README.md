# Chat storage, memory, and messaging channels

**Status:** Planning — v2 not started (as of 2026-06-29)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [e2e-message-crypto](../e2e-message-crypto/) (symmetric E2E body crypto; depends on v2b + v6 for wire-up)

## One-line goal

One durable conversation model for AI and P2P chat: per-thread directories, `ChatPayload` message types, grouped sidebar (AI / Public / Private), multi-level clear sheet, strict seq ingest, durable outbox, and relay seq backfill API.

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Target architecture and desired end state |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today (with pointers) |
| [PHASES.md](PHASES.md) | Phased roadmap and progress checklists |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| — | Baseline (JSON threads, router, sliding context) | Done (pre-project) |
| v2a | Persistence polish + unified transcript | Not started |
| v2b | Public vs E2E channel split + sidebar groups | Not started |
| v3 | Durable AI memory + clear/forget UX | Not started |
| v4 | ChatPayload types + transport badges | Not started |
| v6 | Sender seq + gap detection + relay fetch API | Not started |
| v6b | `@ai` three modes (local / shared reply / full) | Not started |
| v5 | Optional SQLite backend | Deferred |

## Open questions

**None in this project** — O001–O005 resolved as D023–D027.

**Cross-project (e2e-message-crypto):** PSK entry UX (E-O003), automated key agreement (E-O004), group E2E (E-O005).
