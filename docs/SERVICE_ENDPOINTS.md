# Service endpoints

Relay, directory, and registration share a common resolution pattern. All client creation goes through [`CreateServiceClients`](../src/base/net/ServiceClientFactory.cpp) so future libp2p transports can plug in without changing messaging or agent code.

## Resolution order

For each of `relay`, `directory`, and `registration`:

| Priority | Condition | Implementation |
|----------|-----------|----------------|
| 1 | `base_url` non-empty in `config.json` | `Http*Client` |
| 2 | `base_url` empty and promoted MCP client running | `Mcp*Client` (infra tools) |
| 3 | otherwise | `Mock*Client` |

Explicit HTTP URLs always win, even when promoted MCP is connected.

## Config shape

```json
{
  "relay": { "base_url": "", "transport": "http" },
  "directory": { "base_url": "" },
  "registration": { "base_url": "" }
}
```

`transport` is reserved for future libp2p support (`http` | `libp2p`). v1 ignores non-HTTP transports.

## Promoted MCP infra tools

When HTTP URLs are unset, the promoted MCP client bridges native interfaces:

| Native interface | MCP tool | Result shape |
|------------------|----------|--------------|
| `IDirectoryClient::SearchPeople` | `search_people` | JSON array of directory hits |
| `IRegistrationClient::Register` | `register_user` | `{ success, relay_user_id, message }` |
| `IRegistrationClient::UpdateNickname` | `update_profile_nickname` | `{ success, message }` |
| `IRelayClient::Send` | `relay_send` | relay envelope fields |
| `IRelayClient::PollInbox` | `relay_poll_inbox` | `{ messages, next_cursor }` |

These tool names are excluded from the agent tool registry when registered via promoted MCP (native `MessagingTools` own the agent-facing names).

## MCP client buckets

| Bucket | Config | Agent tools |
|--------|--------|-------------|
| Promoted | `promoted_mcp` (+ platform default) | Feed/AI tools; infra tools bridged natively |
| Custom | `mcp_servers[]` | All tools; collisions prefixed as `{id}__{tool}` |

[`McpRuntime`](../src/base/ai/mcp/McpRuntime.cpp) starts one client for promoted MCP and one per enabled `mcp_servers` entry.

## libp2p (deferred)

Future work adds `Libp2p*Client` implementations behind the same interfaces. `ServiceEndpointConfig::transport` and optional peer/multiaddr fields are reserved in config schema; factory selection will branch on `transport` without touching `MessagingHub` or `P2pMessagingService`.
