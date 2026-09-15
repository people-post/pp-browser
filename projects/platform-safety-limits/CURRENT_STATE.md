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

P3 complete: both profile-store files are size-checked before parsing and are read into bounded buffers (4 MiB maximum). Files that exceed the limit fail with an error; no max-contact-count policy was added because O004 remains open.

| Store | Path | Status |
|-------|------|--------|
| `IdentityStore` | `identity.enc` | Encrypted profile payload capped at 4 MiB before decrypt/parse. |
| `ContactsStore` | `contacts.json` | JSON file capped at 4 MiB before parse. |

## MCP (`src/domain/ai/mcp/McpClient.*`, `src/feature/ai/tools/McpToolAdapter.cpp`)

P4 complete: serialized MCP tool results fail closed at 1 MiB before they enter the agent tool-result path.

| Item | Status |
|------|--------|
| Tool result JSON | Capped at 1 MiB after MCP result serialization; oversized results return a stable tool error. |

## AI parser output (`src/domain/ai/StructuredTextParser.cpp`)

| Item | Status |
|------|--------|
| Output RML size | Capped at 512 KiB per parsed turn; overflow follows the existing failed parse result path. |
| Input text | Bounded by the LLM response-body limit. |

## Composer structured actions

| Item | Status |
|------|--------|
| `user_payload` | Capped at 64 KiB at `MessageRouter::Route`; ordinary message text is unaffected. |

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
