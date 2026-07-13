# Chat storage, memory, and messaging channels

**Status:** Waves 1–7 landed in tree (2026-07-06) — Bucket B feature-complete; release hygiene pending  
**Owner:** Hongwei + agents  
**Stable refs:** [P2P_MESSAGING.md](../../docs/architecture/P2P_MESSAGING.md), [WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md), [COMPATIBILITY.md](../../docs/contracts/COMPATIBILITY.md), [AGENT_CONVERSATION.md](../../docs/ui/AGENT_CONVERSATION.md), [DATA_LAYOUT.md](../../docs/contracts/DATA_LAYOUT.md)  
**Related project:** [e2e-message-crypto](../e2e-message-crypto/) (symmetric E2E body crypto; depends on v2b + v6 for wire-up)  
**Recent:** D090 (no `public_relay` / plaintext direct wire) — see [DECISIONS.md](DECISIONS.md)

## One-line goal

One durable conversation model for AI and P2P chat: SQLite per thread, `profile.db` catalog/outbox/`chat_targets` (identity-keyed `ChatTargetKey`, D056/D079), local `thread_id` only, `display_order` paging (D054), ChatPayload v1, **three chat tiers** (private / public / group direct — D089), channel badges, E2E seq sync with **peer-first backfill** (D058–D060) and **user-initiated sync** (D059) — richer types, scroll backfill, shared `@ai`, and transport badges phased after v1.

**Related:** [platform-safety-limits](../platform-safety-limits/) (LLM HTTP, profile stores — outside chat wire).

## Release scope (v1 batch)

**Bucket B — v1 + post-v1 polish** ([D092](DECISIONS.md#d092--release-scope-bucket-b)): chat **v2a–v6** plus [e2e c1–c3](../e2e-message-crypto/PHASES.md) (private `e2e` tier), **plus** post-v4, post-v6b/c/d. Peer-direct history (**D060**) is **required** for v1 ([D094](DECISIONS.md#d094--peer-direct-history-required-for-v1-d060)). Exclude unless explicitly expanded: `e2e_public` auto-key (c3+), group E2E, PQ (c4). See [PHASES § Agent batch delivery](PHASES.md#agent-batch-delivery-order).

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | **Complete system specification** — all behavior with `[v1]` / `[post-v1]` maturity tags |
| [WIRE_SCHEMAS.md](../../docs/contracts/WIRE_SCHEMAS.md) | **Normative wire** (promoted) — envelope JSON, binary ChatPayload (D087), history fetch (D072). Stub left at [WIRE_SCHEMAS.md](WIRE_SCHEMAS.md). |
| [PHASES.md](PHASES.md) | **Implementation order** — checklists, exit criteria, traceability; **[agent batch waves](PHASES.md#agent-batch-delivery-order)** for pre-release delivery |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style rationale) |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the codebase does today |
| [PENDING_DECISIONS.md](../PENDING_DECISIONS.md) | **Human checklist** — scope, relay, libp2p, cross-project open items |

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
| v2a-core | SQLite, `GetMessagesPage`, ChatPayload BLOB | **Done** |
| v2a-p2p | v1 `RelayEnvelope`, `chat_targets`, outbox, reconcile | **Done** |
| v2b | Tier shells, badges, `e2e_public` gated compose | **Done** |
| v3 | Durable AI memory + clear/forget UX | **Done** |
| v4 | ChatPayload validation + transport column | **Done** |
| v6 | E2E sender seq + tail/gap/user sync + integrity | **Done** |
| post-v4 / post-v6b/c/d | Rich types, shared `@ai`, scroll backfill, badges | **Done** (wave 7) |
| e2e c1–c3 | Private `e2e` tier crypto + PSK UX | **Done** (waves 5–6) |

## Open questions

**Human checklist (scope, relay, libp2p):** [PENDING_DECISIONS.md](../PENDING_DECISIONS.md) — **all resolved 2026-07-06** ([D092–D095](DECISIONS.md#d092--release-scope-bucket-b)).

**In this project:** **O008 resolved** → [D095](DECISIONS.md#d095--group-pairwise-wire-shape-o008) (N ciphertexts per message). **O007 resolved** — [e2e E024](../e2e-message-crypto/DECISIONS.md#e024--auto-key-trust-anchor-for-e2e_public-o007) + [D091](DECISIONS.md#d091--blockchain-contact-id-caip-10-e024).

**Cross-project (e2e-message-crypto):** three tiers E021/E022; peer signing keys E016/D081; PSK UX and wire-up after chat-storage **v2b + v6** — see [PHASES.md](PHASES.md) § Cross-project.
