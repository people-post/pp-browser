# Ongoing projects

Work-in-progress design and implementation tracking for pp-browser. Unlike [`docs/`](../docs/), these files are **living project notes**: they change as work proceeds, track checkboxes, and record decisions before they land in stable reference docs.

## How to use (humans and agents)

1. Open the project folder for the feature you are working on.
2. Read **DESIGN.md** (complete spec) and **CURRENT_STATE.md** (today) before coding.
3. Pick tasks from **PHASES.md** (ordering only); for batch pre-release delivery, follow **PHASES § Agent batch delivery** in [chat-storage](chat-storage-and-memory/PHASES.md#agent-batch-delivery-order) and [e2e](e2e-message-crypto/PHASES.md#agent-batch-delivery-order).
4. Mark items done in the same PR that implements them.
5. Log non-obvious choices in **DECISIONS.md** (date + rationale).
6. When a phase ships, update the status line in the project **README.md**.

When a project is fully delivered and stable, fold enduring facts into `docs/` and archive or delete the project folder.

## Active projects

| Project | Status | Summary |
|---------|--------|---------|
| [chat-storage-and-memory](chat-storage-and-memory/) | Planning / early v2 | SQLite per thread, ChatPayload, seq sync, **resource bounds D029–D033** |
| [platform-safety-limits](platform-safety-limits/) | Planning | LLM HTTP, profile JSON, MCP, parser output — non-chat limits |
| [e2e-message-crypto](e2e-message-crypto/) | d0 complete → c1 next | Manual PSK E2E: libsodium, XChaCha20-Poly1305, HKDF; spec in [docs/MESSAGE_ENCRYPTION.md](../docs/MESSAGE_ENCRYPTION.md) |
