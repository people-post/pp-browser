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
  - `user_text`, optional `user_payload` (LLM-only structured JSON fence), `assistant_raw` (LLM context)
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

## Dual-channel user messages (`user_payload`)

Some UI actions (notably **form submit**) send two representations of the same user turn:

| Channel | Field | Audience |
|---------|-------|----------|
| Display | `user_text` | Chat bubbles (human-readable) |
| Structured | `user_payload` | LLM context only (JSON string) |

`SlidingWindowContextPolicy` formats user messages for the model via `FormatUserContentForLlm()`: when `user_payload` is set, the LLM sees `user_text` plus a fenced ` ```json ` block. The UI continues to render `user_text` only.

`Conversation::AppendUser(text, payload)` and `AgentSession::Submit(text, payload)` accept the optional payload.

## In-chat forms (single active form)

Chat forms are inline RML inside assistant bubbles (see `ChatFormHelper`, `ChatDemo`):

| Rule | Behavior |
|------|----------|
| Single active form | Only the latest unsubmitted form on the current assistant turn is editable |
| Expire on progression | Sending any user message or submitting a form disables older forms |
| Re-offer | The model may emit a new form in a later assistant reply; that becomes the new active form |
| Submit | `submit_form(entry_id, form_id)` reads DOM values, sends `user_text` from `data-submit-template`, and `user_payload` as `{"type":"form_submission",...}` |

Mock chat: type `form` (without LLM configured) to exercise the booking form sample.

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
