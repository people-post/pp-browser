# Ongoing projects

Work-in-progress design and implementation tracking for pp-browser. Unlike [`docs/`](../docs/), these files are **living project notes**: they change as work proceeds, track checkboxes, and record decisions before they land in stable reference docs.

## How to use (humans and agents)

1. Open the project folder for the feature you are working on.
2. Read **DESIGN.md** (target) and **CURRENT_STATE.md** (today) before coding.
3. Pick tasks from **PHASES.md**; mark items done in the same PR that implements them.
4. Log non-obvious choices in **DECISIONS.md** (date + rationale).
5. When a phase ships, update the status line in the project **README.md**.

When a project is fully delivered and stable, fold enduring facts into `docs/` and archive or delete the project folder.

## Active projects

| Project | Status | Summary |
|---------|--------|---------|
| [chat-storage-and-memory](chat-storage-and-memory/) | Planning / early v2 | SQLite per thread, ChatPayload, seq sync, **resource bounds D029–D033** |
| [platform-safety-limits](platform-safety-limits/) | Planning | LLM HTTP, profile JSON, MCP, parser output — non-chat limits |
| [e2e-message-crypto](e2e-message-crypto/) | Planning / design (d0) | Manual PSK E2E: libsodium, XChaCha20-Poly1305, HKDF, AAD + replay binding; PQ posture; `base/crypto` groundwork before messaging wire-up |
