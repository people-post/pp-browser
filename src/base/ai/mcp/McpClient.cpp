#include "base/ai/mcp/McpClient.h"

#include "base/platform/os/OsProcess.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

HttpPostFn& McpClient::HttpPostSlot() {
  static HttpPostFn post;
  return post;
}

void McpClient::SetHttpPost(HttpPostFn post) {
  HttpPostSlot() = std::move(post);
}

namespace {

Object EmptyObject() {
  return {};
}

Object MakeStringPropsQuerySchema() {
  Object query;
  query.set("type", "string");
  Object properties;
  properties.set("query", query);
  Object schema;
  schema.set("type", "object");
  schema.set("properties", properties);
  schema.set("required", ArrayValue({Value(std::string("query"))}));
  return schema;
}

} // namespace

McpClient::McpClient() {
  redirectLogger("McpClient");
}

McpClient::~McpClient() {
  Stop();
}

void McpClient::Stop() {
  if (!running_) {
    return;
  }
  process_.Stop();
  running_ = false;
  mock_ = false;
  http_ = false;
  http_url_.clear();
}

bool McpClient::Start(const std::string& command, const std::vector<std::string>& args) {
  Stop();
  if (command == "mock") {
    log().info << "Starting mock MCP client";
    mock_ = true;
    running_ = true;
    return true;
  }
  if (!process_.Start(command, args)) {
    mock_ = true;
    running_ = true;
    return true;
  }
  running_ = true;
  log().info << "Started MCP process: " << command;
  return true;
}

bool McpClient::StartHttp(const std::string& url) {
  Stop();
  if (url.empty()) {
    log().error << "HTTP MCP URL is empty";
    return false;
  }
  http_url_ = url;
  http_ = true;
  running_ = true;
  log().info << "Connected to HTTP MCP: " << url;
  return true;
}

Roe<Object> McpClient::Request(const std::string& method, const Object& params) {
  if (mock_) {
    if (method == "initialize") {
      Object out;
      out.set("protocolVersion", "2024-11-05");
      out.set("capabilities", EmptyObject());
      return out;
    }
    if (method == "tools/list") {
      Object tool;
      tool.set("name", "user_search");
      tool.set("description", "Search users by query");
      tool.set("inputSchema", MakeStringPropsQuerySchema());
      Object out;
      out.set("tools", ArrayValue({ObjectValue(std::move(tool))}));
      return out;
    }
    if (method == "tools/call") {
      const auto name = params.getString("name").value_or("");
      if (name == "user_search") {
        Object text_block;
        text_block.set("type", "text");
        text_block.set("text", R"([{"name":"Ada","email":"ada@example.com"}])");
        Object out;
        out.set("content", ArrayValue({ObjectValue(std::move(text_block))}));
        return out;
      }
    }
    return EmptyObject();
  }

  if (http_) {
    Object req;
    req.set("jsonrpc", "2.0");
    req.set("id", static_cast<int64_t>(request_id_++));
    req.set("method", method);
    req.set("params", params);

    log().debug << "HTTP MCP request: " << method;
    if (!HttpPostSlot()) {
      return Error("MCP HTTP transport is not configured");
    }
    auto response = HttpPostSlot()(http_url_, DumpJson(req), {{"Content-Type", "application/json"}});
    if (!response) {
      log().error << "HTTP MCP request failed for " << method << ": " << response.error().message;
      return response.error();
    }
    if (response->status_code < 200 || response->status_code >= 300) {
      log().error << "HTTP MCP status " << response->status_code << " for " << method;
      return Error("HTTP MCP status " + std::to_string(response->status_code));
    }
    if (response->body.empty()) {
      return EmptyObject();
    }

    auto resp_value = ParseValue(response->body);
    if (!resp_value) {
      log().error << "HTTP MCP response parse failed for method: " << method;
      return Error("MCP response parse failed");
    }
    const Object* resp = asObject(*resp_value);
    if (!resp) {
      log().error << "HTTP MCP response parse failed for method: " << method;
      return Error("MCP response parse failed");
    }
    if (auto err_slot = resp->fields().tryGet("error")) {
      const std::string err_text = DumpJson(err_slot->get());
      log().error << "MCP error for " << method << ": " << err_text;
      return Error(err_text);
    }
    if (const Object* result = resp->getObject("result")) {
      return *result;
    }
    return EmptyObject();
  }

  if (!process_.IsActive()) {
    return EmptyObject();
  }

  Object req;
  req.set("jsonrpc", "2.0");
  req.set("id", static_cast<int64_t>(request_id_++));
  req.set("method", method);
  req.set("params", params);

  const std::string payload = DumpJson(req) + "\n";
  log().debug << "MCP request: " << method;
  if (process_.Write(payload.data(), payload.size()) < 0) {
    log().error << "MCP write failed for method: " << method;
    return Error("MCP write failed");
  }

  std::string line;
  char ch = 0;
  while (process_.Read(&ch, 1) == 1) {
    if (ch == '\n') {
      break;
    }
    line.push_back(ch);
  }
  if (line.empty()) {
    log().error << "MCP empty response for method: " << method;
    return Error("MCP empty response");
  }

  auto resp_value = ParseValue(line);
  if (!resp_value) {
    log().error << "MCP response parse failed for method: " << method;
    return Error("MCP response parse failed");
  }
  const Object* resp = asObject(*resp_value);
  if (!resp) {
    log().error << "MCP response parse failed for method: " << method;
    return Error("MCP response parse failed");
  }

  if (auto err_slot = resp->fields().tryGet("error")) {
    const std::string err_text = DumpJson(err_slot->get());
    log().error << "MCP error for " << method << ": " << err_text;
    return Error(err_text);
  }
  if (const Object* result = resp->getObject("result")) {
    return *result;
  }
  return EmptyObject();
}

Roe<void> McpClient::Initialize() {
  Object params;
  params.set("protocolVersion", "2024-11-05");
  params.set("capabilities", EmptyObject());
  auto result = Request("initialize", params);
  if (!result) {
    log().error << "MCP initialize failed: " << result.error().message;
    return result.error();
  }
  log().info << "MCP initialized";
  return {};
}

Roe<std::vector<McpTool>> McpClient::ListTools() {
  auto result = Request("tools/list", EmptyObject());
  if (!result) {
    return result.error();
  }

  std::vector<McpTool> tools;
  if (const Array* tool_array = result->getArray("tools")) {
    for (const Value& tool_value : tool_array->elements) {
      const Object* tool = asObject(tool_value);
      if (!tool) {
        continue;
      }
      McpTool entry;
      entry.name = tool->getString("name").value_or("");
      entry.description = tool->getString("description").value_or("");
      if (const Object* schema = tool->getObject("inputSchema")) {
        entry.input_schema = *schema;
      }
      tools.push_back(std::move(entry));
    }
  }
  return tools;
}

Roe<Object> McpClient::CallTool(const std::string& name, const Object& arguments) {
  Object params;
  params.set("name", name);
  params.set("arguments", arguments);
  return Request("tools/call", params);
}

McpClient& McpClient::MockInstance() {
  static McpClient client;
  if (!client.running_) {
    client.Start("mock");
    client.Initialize();
  }
  return client;
}

} // namespace pbr
