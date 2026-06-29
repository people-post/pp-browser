# Platform safety limits (non-chat)

**Status:** Planning — not started (as of 2026-06-29)  
**Owner:** Hongwei + agents  

Bounds and hardening for **non-chat** subsystems: LLM HTTP, profile JSON stores, MCP bridges, and local AI parser output. Chat wire, thread store, relay ingest, and P2P UI limits live in [chat-storage-and-memory](../chat-storage-and-memory/) (D029–D033).

## One-line goal

Prevent memory exhaustion and unsafe parsing in shared platform layers without duplicating chat-specific envelope rules.

## Documents

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Target limits and enforcement points |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Gaps in codebase today |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style) |
| [PHASES.md](PHASES.md) | When to implement (mostly independent of chat v2a) |

## Related projects

| Project | Scope |
|---------|--------|
| [chat-storage-and-memory](../chat-storage-and-memory/) | Message size, relay poll, transcript window, ingest |
| [e2e-message-crypto](../e2e-message-crypto/) | PSK at rest, AEAD plaintext caps (cross-ref D029 E2E row) |

## Progress

| Track | Status |
|-------|--------|
| Design baseline | Done |
| Implementation | Not started |
