# Agent conversation model

pp-browser chat uses a **conversation-first** design: one shared transcript drives both the UI and LLM context. The goal is natural follow-up chat without the model forgetting prior turns.

## Principles

| User expectation | Design response |
|------------------|-----------------|
| Follow-ups work | Durable transcript fed into every LLM call |
| New chat = fresh start | `StartNewConversation()` clears transcript |
| Long threads stay usable | Sliding-window context policy trims oldest turns |
| Tool noise stays invisible | Tool-call messages live in ephemeral turn buffer only |
| UI and model stay aligned | One `Conversation` object, dual render fields per turn |

## Three layers

```
Transcript (Conversation)     — user_text + assistant_raw, cross-turn
Context view (IContextPolicy) — system + trimmed history + current user, rebuilt each turn
Turn execution (turn_scratch) — tool calls / search injections, current turn only
```

## Core types

[`src/agent/conversation/`](../src/agent/conversation/)

- **`TranscriptEntry`** — one user message and optional assistant reply
  - `user_text`, `assistant_raw` (LLM context)
  - `assistant_rml`, `suggestions` (UI, filled after parsing)
- **`Conversation`** — in-memory transcript + optional `ConversationSummary`
- **`IContextPolicy`** — builds LLM message list from transcript
- **`SlidingWindowContextPolicy`** — v1 policy: recent turn pairs + char/token budgets
- **`TurnCoordinator`** — `BeginTurn` / `CompleteTurn` orchestration

## Turn flow

1. User sends → `Conversation::AppendUser`
2. `TurnCoordinator::BeginTurn` → context policy builds messages → `AgentSession::turn_scratch_`
3. Tool loop extends `turn_scratch_` only
4. Final assistant text → `CompleteTurn` (stores `assistant_raw`) → UI parses → `SetAssistantDisplay`

## Configuration

Optional `context` section in `config.json`:

```json
{
  "context": {
    "max_turn_pairs": 6,
    "max_recent_chars": 6000,
    "max_input_tokens": 8000,
    "token_estimate_margin": 0.85,
    "max_summary_chars": 2000
  }
}
```

Defaults apply when omitted (see `DefaultContextBudget()` in `ConversationTypes.h`).

## Relation to user-ai

Inspired by user-ai turn-pair windowing and prepare/complete turn split. Simplified for local 1:1 chat:

- **Kept:** sliding window, char budgets, turn index, session id
- **Deferred:** curation, retractions, digest consolidation, retrieval — add via new `IContextPolicy` or `ICompactionService` when needed

## Extension roadmap

| Phase | Work |
|-------|------|
| v1 | Conversation + sliding context + New chat |
| v2 | `IConversationStore` for disk persistence |
| v2 | Multi-session sidebar switching |
| v3 | `ICompactionService` → `ConversationSummary` for very long threads |
| v3 | Transcript edit / "forget that" APIs |
| v4 | Retrieval-augmented `IContextPolicy` |
