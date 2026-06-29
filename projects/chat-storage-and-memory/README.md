# Chat storage, memory, and messaging channels

**Status:** Planning — v2 not started (as of 2026-06-27)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [e2e-message-crypto](../e2e-message-crypto/) (symmetric E2E body crypto; depends on v2b + v6 for wire-up)

## One-line goal

One durable, query-friendly conversation model for AI and person-to-person chat, with explicit clear/forget semantics, separate public vs encrypted channels per contact, stable message IDs, sender sequence with **strict normal-or-compromised ingest** in private chat (D013), epoch-scoped peer reset (D014), three `@ai` modes in direct threads (local / shared reply / shared full), and visible transport provenance in private mode.

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
| v2b | Public vs E2E channel split | Not started |
| v3 | Durable AI memory + clear/forget UX | Not started |
| v4 | Annotations + transport badges | Not started |
| v6 | Sender seq + gap detection + windowed sync | Not started |
| v6b | `@ai` three modes (local / shared reply / full) | Not started |
| v5 | Optional SQLite backend | Deferred |

Update this table when a phase completes.

## Open questions

- [ ] Sidebar UX for two threads per contact (two rows vs mode toggle)?
- [ ] Should “clear history” on P2P warn that peer/relay may retain copies?
- [ ] Relay protocol version for `annotation` envelope fields?
- [ ] When to promote `ConversationSummary` to disk vs keep in thread JSON?
- [ ] Relay API for tail/gap backfill when peer is offline (O005)?
- [ ] E2E ciphertext field name and sign-payload canonicalization — coordinate with [e2e-message-crypto](../e2e-message-crypto/DECISIONS.md) O001
