# Design — platform resource bounds (non-chat)

## Scope

This project covers limits **outside** the chat wire / thread store / relay ingest path. See [chat-storage-and-memory D029–D033](../chat-storage-and-memory/DECISIONS.md) for compose, `ChatPayload`, envelope, poll, and transcript window rules.

## Principles

1. **Fail closed on oversize untrusted input** — HTTP responses and on-disk JSON from external or stale sources.
2. **Shared constants** — `PlatformLimits.h` in `src/common/` or `src/base/` (name TBD); chat code may include but not duplicate LLM caps.
3. **Check size before parse** — read `Content-Length` or stream cap before `nlohmann::json::parse` where feasible.

## Target limits

| Constant | Value | Applies to |
|----------|-------|------------|
| `kMaxLlmResponseBytes` | **8 MiB** | Single LLM HTTP response body (`LlmClient`) |
| `kMaxLlmRequestBytes` | **2 MiB** | Outbound chat-completions JSON (tools + messages) |
| `kMaxProfileJsonFileBytes` | **4 MiB** | `identity.json`, `contacts.json`, config reads |
| `kMaxMcpToolResultBytes` | **1 MiB** | MCP tool JSON payloads |
| `kMaxStructuredParserOutputBytes` | **512 KiB** | `StructuredTextParser` output RML per turn |
| `kMaxUserPayloadBytes` | **64 KiB** | Composer structured action JSON (`user_payload`) |
| `kMaxDirectorySearchResults` | **100** | Directory client result array |
| `kMaxHttpClientBodyBytes` | **8 MiB** | Default cap for `HttpClient` GET/POST bodies (non-relay override) |

Relay-specific caps (`kMaxRelayEnvelopeJsonBytes` = 256 KiB) stay in **chat-storage** — `HttpRelayClient` should use the chat constant, not `kMaxHttpClientBodyBytes`.

## Enforcement map

```
┌─────────────────────────────────────────────────────────────┐
│ LlmClient::Complete                                         │
│   cap response WriteCallback; cap request body.dump()       │
├─────────────────────────────────────────────────────────────┤
│ HttpClient (base)                                           │
│   optional max_body per request; default kMaxHttpClientBody   │
├─────────────────────────────────────────────────────────────┤
│ IdentityStore / ContactsStore                               │
│   reject file > kMaxProfileJsonFileBytes before parse       │
├─────────────────────────────────────────────────────────────┤
│ McpClient / CallMcpToolJson                                 │
│   cap tool result string                                    │
├─────────────────────────────────────────────────────────────┤
│ StructuredTextParser                                        │
│   cap output RML length before persist to thread store      │
├─────────────────────────────────────────────────────────────┤
│ MessageRouter / ChatController                              │
│   cap user_payload size (chat compose path)                 │
└─────────────────────────────────────────────────────────────┘
```

## Agent session bounds (existing — document only)

Already coded; keep documented here for cross-reference:

| Limit | Value | Location |
|-------|-------|----------|
| Max agent iterations | 8 | `AgentSession.cpp` `kMaxIterations` |
| Max tools per turn plan | 4 | `TurnPlan.cpp` `kMaxPlannedTools` |
| Context budget defaults | 6 pairs / 6000 chars / 8000 tokens | `ConversationTypes.h` `ContextBudget` |

## PSK / identity at rest

Not in this project's implementation track — see [e2e-message-crypto E008](../e2e-message-crypto/DECISIONS.md) (PSK JSON) and future keychain work. Do apply `kMaxProfileJsonFileBytes` to `identity.json` reads.

## Non-goals

- libp2p transport frame sizes (fork-owned; SECIO ~8 MiB today)
- RmlUi document size limits
- Chat relay ingest (chat-storage project)

## Success criteria

- [ ] `LlmClient` rejects or truncates responses > 8 MiB with clear error.
- [ ] `contacts.json` / `identity.json` load fails gracefully when file exceeds 4 MiB.
- [ ] `HttpClient` supports per-call body ceiling; relay uses chat limit.
- [ ] `StructuredTextParser` fails closed on excessive output before writing `content_rml`.
- [ ] Constants documented in one header; unit tests for boundary ±1 byte.
