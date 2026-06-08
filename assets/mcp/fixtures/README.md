# MCP fixtures

Use the built-in mock MCP client during development:

```cpp
McpClient client;
client.Start("mock");
client.Initialize();
```

For a real stdio MCP server on Unix, configure `command` and `args` in `config.json` and call `McpClient::Start`.
