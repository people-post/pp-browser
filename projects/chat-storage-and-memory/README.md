# Chat storage, memory, and messaging channels

**Status:** Waves 1–2 landed in tree (2026-07-02) — **Wave 3 next** (v3 ∥ v4)  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/P2P_MESSAGING.md), [AGENT_CONVERSATION.md](../../docs/AGENT_CONVERSATION.md), [CONFIGURATION.md](../../docs/CONFIGURATION.md)  
**Related project:** [e2e-message-crypto](../e2e-message-crypto/) (symmetric E2E body crypto; depends on v2b + v6 for wire-up)  
**Recent:** D090 (no `public_relay` / plaintext direct wire) — see [DECISIONS.md](DECISIONS.md)

## One-line goal

One durable conversation model for AI and P2P chat: SQLite per thread, `profile.db` catalog/outbox/`chat_targets` (identity-keyed `ChatTargetKey`, D056/D079), local `thread_id` only, `display_order` paging (D054), ChatPayload v1, **three chat tiers** (private / public / group direct — D089), channel badges, E2E seq sync with **peer-first backfill** (D058–D060) and **user-initiated sync** (D059) — richer types, scroll backfill, shared `@ai`, and transport badges phased after v1.

**Related:** [platform-safety-limits](../platform-safety-limits/) (LLM HTTP, profile stores — outside chat wire).

## Release scope (v1 batch)

Before the first customer release, agents implement **chat v2a–v6** plus [e2e c1–c3](../e2e-message-crypto/PHASES.md) (private `e2e` tier). Exclude unless explicitly expanded: post-v4/6b/c/d, `e2e_public` auto-key (c3+), group E2E (O008), PQ (c4). See [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | **Complete system specification** — all behavior with `[v1]` / `[post-v1]` maturity tags |
| [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md) | **Normative wire shapes** — envelope JSON, binary ChatPayload (D087), history fetch (D072) |
| [PHASES.md](PHASES.md) | **Implementation order** — checklists, exit criteria, traceability; **[agent batch waves](PHASES.md#agent-batch-delivery-order)** for pre-release delivery |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style rationale) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |

## v1 implementation order

See [PHASES.md](PHASES.md) for full checklists. For **agent batch delivery** (all phases before one release), use [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order). Rollout summary:

| Phase | Focus |
|-------|--------|
| v2a | SqliteThreadStore, reconciliation, `GetMessagesPage`, `ChatTargetKey` routing, clear history + **confirmation dialog** (D057) |
| v2b | Private vs public tier **shells** + badges (`e2e_public` gated until c3) |
| v3 | AI memory + compaction |
| v4 | ChatPayload text/system, transport column |
| v6 | E2E seq, tail sync, gap repair, **user sync**, integrity UX |

`[post-v1]` work (post-v4, post-v6b–e) is specified inline in DESIGN and scheduled in PHASES.

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| — | Baseline (JSON `JsonThreadStore`, router, sliding context) | Done (pre-project) |
| v2a-core | SQLite, `GetMessagesPage`, ChatPayload BLOB, feature off `GetMessages` | **Done** (UX gaps: clear confirm, maxlength) |
| v2a-p2p | v1 `RelayEnvelope`, `chat_targets`, outbox, reconcile | **Done** (signing interim JSON; plaintext `payload_b64` until c2) |
| v2b | Tier shells, badges, `e2e_public` gated compose | **Done** |
| v3 | Durable AI memory + clear/forget UX | Not started — **Wave 3** |
| v4 | ChatPayload text/system validation + transport column | Not started — **Wave 3** |
| v6 | E2E sender seq + tail/gap/user sync + integrity | Not started — **Wave 4** |

## Open questions

**In this project:** O008 (group pairwise wire shape) — see [DECISIONS.md](DECISIONS.md#open-decisions-not-yet-resolved). **O007 resolved** — [e2e E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007) + [D091](DECISIONS.md#d091--blockchain-contact-id-caip-10-e024).

**Cross-project (e2e-message-crypto):** three tiers E021/E022; peer signing keys E016/D081; PSK UX and wire-up after chat-storage **v2b + v6** — see [PHASES.md](PHASES.md) § Cross-project.
