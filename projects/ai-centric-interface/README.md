# AI-centric interface (intent, tools, agency)

**Status:** Design baseline (d0) — planning  
**Owner:** Hongwei + agents  
**Stable refs:** [AGENT_CONVERSATION.md](../../docs/ui/AGENT_CONVERSATION.md), [AGENTS.md](../../AGENTS.md)  
**Related:** [chat-storage-and-memory](../chat-storage-and-memory/) (transcript / memory), [platform-safety-limits](../platform-safety-limits/) (LLM/MCP caps)

## One-line goal

Make pp-browser (and later the broader product) an **AI-centric shell**: the user talks to the interface to interact with **anything** — people, content, apps, devices, identity — via a stable intent model, tools, and clear agency (suggest / confirm / execute / autonomous).

## Strategy

1. **Document the long-term taxonomy now** (acts closed, domains open).
2. **v1: every act exists** with the simplest workable path (even stubs that clarify limits).
3. **Improve one act/domain at a time** without rewriting the model.

Completeness of the taxonomy beats early sophistication of any single path.

## Documents in this folder

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Long-term intent model, agency, v1 simple coverage matrix |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What the agent/planner/tools do today |
| [PHASES.md](PHASES.md) | Implementation order — d0 → v1 thin coverage → per-act upgrades |
| [DECISIONS.md](DECISIONS.md) | Recorded decisions (ADR-style) |

## Progress snapshot

| Phase | Name | Status |
|-------|------|--------|
| d0 | Design baseline (this folder) | **In progress** |
| v1 | Thin coverage for all 10 acts + planner wiring | Not started |
| v1+ | Per-act / per-domain upgrades | Deferred until v1 exits |

## Motivating gap

Natural language like *“Add contact of this peer id: 12D3KooW…”* fails today because planning is **Discover-shaped** (search → list chips) while the user asked for **Operate**. See [CURRENT_STATE.md](CURRENT_STATE.md) and [DESIGN.md](DESIGN.md) § Motivating example.
