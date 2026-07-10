# Ongoing projects

Work-in-progress design and implementation tracking for pp-browser. Unlike [`docs/`](../docs/), these files are **living project notes**: they change as work proceeds, track checkboxes, and record decisions before they land in stable reference docs.

## How to use (humans and agents)

1. Open the project folder for the feature you are working on.
2. Read **DESIGN.md** (complete spec) and **CURRENT_STATE.md** (today) before coding.
3. **Human:** unresolved rollout choices live in **[PENDING_DECISIONS.md](PENDING_DECISIONS.md)** — resolve before expanding scope or v6-sync exit criteria.
3. Pick tasks from **PHASES.md** (ordering only); for batch pre-release delivery, follow **PHASES § Agent batch delivery** in [chat-storage](chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) and [e2e](e2e-message-crypto/PHASES.md#agent-batch-delivery-order).
4. Mark items done in the same PR that implements them.
5. Log non-obvious choices in **DECISIONS.md** (date + rationale).
6. When a phase ships, update the status line in the project **README.md**.

When a project is fully delivered and stable, fold enduring facts into `docs/` and archive or delete the project folder.

## Active projects

| Project | Status | Summary |
|---------|--------|---------|
| [ai-centric-interface](ai-centric-interface/) | **d0 design** — v1 next | Intent taxonomy (10 acts), agency, planner/tools — thin path for every act first — [CURRENT_STATE](ai-centric-interface/CURRENT_STATE.md) |
| [chat-storage-and-memory](chat-storage-and-memory/) | **Waves 1–2 + v3 core done** — v4 next | SQLite, v1 relay, tier shells, memory/compaction — see [CURRENT_STATE § Next agent](chat-storage-and-memory/CURRENT_STATE.md#next-agent--start-here) |
| [platform-safety-limits](platform-safety-limits/) | Planning | LLM HTTP, profile JSON, MCP, parser output — non-chat limits |
| [e2e-message-crypto](e2e-message-crypto/) | **c1 done** — c2 after chat v6 | `base/crypto` + vectors; AEAD on wire in c2 — [CURRENT_STATE](e2e-message-crypto/CURRENT_STATE.md) |
