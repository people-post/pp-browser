# MCP fixtures

There is no built-in mock MCP server (PB-081). Tests drive the client's transports directly
(`src/lib/ai/mcp/tests/`: stdio framing from scripted bytes, HTTP through an injected post).

For a real stdio MCP server on Unix, configure `command` and `args` in `config.json`; for a remote
one, `url`.
