# Current state — as of 2026-09-16

Gaps in **non-chat** platform layers. Chat-specific issues are in [chat-storage-and-memory/CURRENT_STATE.md](../chat-storage-and-memory/CURRENT_STATE.md).

## LLM (`src/domain/ai/LlmClient.cpp`)

P2 complete: request and response bodies now fail closed at their shared platform limits.

| Item | Status |
|-------|--------|
| Response body size | Capped at 8 MiB in the libcurl write callback; oversized responses return a clear HTTP error. |
| Request body size | Capped at 2 MiB after JSON serialization and before curl is initialized. |
| Timeout | 120 s only |
| Parse | Parses only the bounded response body. |

## HTTP client (`src/domain/net/HttpClient.cpp`)

| Item | Status |
|-------|--------|
| Response body | Capped at 8 MiB by default; callers may set a per-request ceiling or explicitly opt out. |
| Request body | No client-side limit |
| Timeout | 30 s |
| Relay vs generic | Chat relay keeps its existing message limits and can set its own HTTP response ceiling. |

## Profile JSON stores

| Store | Path | Issue |
|-------|------|--------|
| `IdentityStore` | `identity.json` | Full file parse into memory; no size cap; private key base64 at rest |
| `ContactsStore` | `contacts.json` | Full array load; no max contacts or file size |

## MCP (`src/base/net/McpClient.*`, relay/directory bridges)

| Issue | Today |
|-------|--------|
| Tool result JSON | No documented max size |
| MCP relay send | Passes envelope JSON through without size check |

## AI parser output (`src/base/ai/StructuredTextParser.cpp`)

| Issue | Today |
|-------|--------|
| Output RML size | **Unbounded** — can produce large `content_rml` for assistant bubbles |
| Input text | Unbounded LLM content string |

## Composer structured actions

| Issue | Today |
|-------|--------|
| `user_payload` | No size limit in `MessageRouter` / `ChatController` |

## Directory / registration HTTP clients

| Issue | Today |
|-------|--------|
| Search results | Parsed array with no max length |
| Registration POST | Small fixed JSON — low risk |

## Agent session (documented — partially bounded)

| Limit | Status |
|-------|--------|
| `kMaxIterations = 8` | Implemented |
| `kMaxPlannedTools = 4` | Implemented |
| `ContextBudget` defaults | Implemented in config |

## Cross-references

- Chat message / envelope limits → [chat-storage D029](../chat-storage-and-memory/DECISIONS.md)
- E2E plaintext cap → chat D029 `kMaxE2ePlaintextBytes` + e2e crypto
- PSK storage hardening → [e2e-message-crypto](../e2e-message-crypto/)
