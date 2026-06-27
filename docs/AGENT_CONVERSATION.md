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

[`src/base/ai/conversation/`](../src/base/ai/conversation/)

- **`TranscriptEntry`** — one user message and optional assistant reply
  - `user_text`, optional `user_payload` (LLM-only structured JSON fence), `assistant_raw` (LLM context)
  - `assistant_rml`, `chat_actions` (click handlers for chips; UI, filled after parsing)
- **`Conversation`** — in-memory transcript + optional `ConversationSummary`
- **`IContextPolicy`** — builds LLM message list from transcript
- **`SlidingWindowContextPolicy`** — v1 policy: recent turn pairs + char/token budgets
- **`TurnCoordinator`** — `BeginTurn` / `CompleteTurn` orchestration

## Turn flow

1. User sends → transcript append (conversation or thread store)
2. Context policy builds messages → `AgentSession::turn_scratch`
3. **Plan** — `PayloadTurnPlanBuilder` for known UI payloads, else `TurnPlanner` (LLM)
4. **Execute** — `TurnExecutor` runs planned tools into `turn_scratch`
5. **Synthesize** — synthesis LLM produces blocks JSON (skipped when `render_mode: people_list`)
6. **Validate** — `StructuredTextParser`; one output repair retry on parse failure
7. Final assistant text → transcript / thread store → UI parses → `SetAssistantDisplay`

Natural-language turns add one planner LLM call. Structured UI actions (form submit, article chips, pagination) skip the planner via the payload fast path.

## Turn planning

Each agent turn produces a [`TurnPlan`](../src/base/ai/TurnPlan.h):

| Field | Purpose |
|-------|---------|
| `source` | `payload` (deterministic) or `planner` (LLM) |
| `response_goal` | `display_feed`, `summarize`, `answer_question`, `headlines`, `people_discovery`, `general` |
| `tools` | Ordered tool calls the runtime executes before synthesis |
| `render_mode` | `blocks` (default) or `people_list` (deterministic long_list, skips synthesis) |
| `synthesis_hints` | Per-turn guidance for the synthesizer |

**Payload fast path** — [`PayloadTurnPlanBuilder`](../src/feature/ai/PayloadTurnPlanBuilder.cpp) maps known `user_payload` shapes (article actions, `blog_articles` pagination, form submissions, chip tool payloads) without an LLM call.

**NL path** — [`TurnPlanner`](../src/base/ai/TurnPlanner.cpp) emits a JSON plan; one repair retry on invalid output.

[`AgentSession`](../src/feature/ai/AgentSession.cpp) runs plan → execute → synthesize → validate. The synthesis LLM may request additional tools (refinement loop) when planned results are insufficient. Per-turn metrics are logged via [`TurnTrace`](../src/base/ai/TurnTrace.h).

| Goal | Typical source | Expected reply shape |
|------|----------------|----------------------|
| `display_feed` | Article list / pagination payloads | `long_list` + short intro |
| `summarize` | Article action payloads | Heading + concise paragraph/card |
| `answer_question` | Planner (NL) | Answer paragraph first; sources as support |
| `headlines` | Planner (NL) | `list` of real headlines |
| `people_discovery` | Planner or people chip payloads | `long_list` with Message/Add chips |
| `general` | Fallback | Blocks that best serve the ask |

## Dual-channel user messages (`user_payload`)

Some UI actions (notably **form submit**) send two representations of the same user turn:

| Channel | Field | Audience |
|---------|-------|----------|
| Display | `user_text` | Chat bubbles (human-readable) |
| Structured | `user_payload` | LLM context only (JSON string) |

`SlidingWindowContextPolicy` formats user messages for the model via `FormatUserContentForLlm()`: when `user_payload` is set, the LLM sees `user_text` plus a hint that `user_text` is primary and a fenced ` ```json ` block for the structured payload. The UI continues to render `user_text` only.

`Conversation::AppendUser(text, payload)` and `AgentSession::Submit(text, payload)` accept the optional payload.

## In-chat forms (single active form)

Chat forms are inline RML inside assistant bubbles (see `ChatFormHelper`, `ChatController`):

| Rule | Behavior |
|------|----------|
| Single active form | Only the latest unsubmitted form on the current assistant turn is editable |
| Expire on progression | Sending any user message or submitting a form disables older forms |
| Re-offer | The model may emit a new form in a later assistant reply; that becomes the new active form |
| Submit | `submit_form(entry_id, form_id)` reads DOM values, sends `user_text` from `data-submit-template`, and `user_payload` as `{"type":"form_submission",...}` |

### Chat actions (indexed dispatcher)

Suggestion chips use `send_chat_action(entry_id, index)` instead of embedding message text in RML. The transcript stores `chat_actions[]` on each assistant turn (`label`, `message`, optional `payload`). `__ENTRY__` in parser output is replaced with the real entry id at display hydration.

Legacy inline `send_suggestion('...')` in stored RML still works for older turns.

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
