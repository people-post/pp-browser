#include "lib/ai/mcp/McpClient.h"

#include "foundation/platform/os/OsProcess.h"
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
  stdio_carry_.clear();
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
    // A server that cannot start is a failure the user sees, never a silent switch to the mock (PB-056).
    log().error << "Could not start MCP process: " << command;
    return false;
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

Roe<Object> McpClient::Request(const std::string& method, const Object& params, const long timeout_s) {
  answered_ = mock_;
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
    auto response = HttpPostSlot()(http_url_, DumpJson(req), {{"Content-Type", "application/json"}}, timeout_s);
    if (!response) {
      log().error << "HTTP MCP request failed for " << method << ": " << response.error().message;
      return response.error();
    }
    answered_ = true;
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
    return Error("MCP process is not running");
  }

  const int64_t id = request_id_++;
  Object req;
  req.set("jsonrpc", "2.0");
  req.set("id", id);
  req.set("method", method);
  req.set("params", params);

  const std::string payload = DumpJson(req) + "\n";
  log().debug << "MCP request: " << method;
  if (process_.Write(payload.data(), payload.size()) < 0) {
    log().error << "MCP write failed for method: " << method;
    return Error("MCP write failed");
  }

  auto response = ReadStdioResponse(
      [this](char* out, const size_t size) { return process_.Read(out, size, kMcpReadTimeoutMs); }, id, stdio_carry_);
  if (!response) {
    // The stream is no longer trustworthy (half a line, a stuck server): stop it rather than misread later.
    log().error << "MCP " << method << " failed: " << response.error().message << "; stopping the server";
    Stop();
    return response.error();
  }
  answered_ = true;
  const Object* resp = &*response;

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

Roe<std::vector<McpTool>> McpClient::ListTools(const long timeout_s) {
  auto result = Request("tools/list", EmptyObject(), timeout_s);
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
      if (const Object* annotations = tool->getObject("annotations")) {
        entry.annotations.read_only_hint = annotations->getIf<bool>("readOnlyHint");
        entry.annotations.destructive_hint = annotations->getIf<bool>("destructiveHint");
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

namespace {

bool IdMatches(const Object& response, const int64_t expected) {
  if (const auto i = response.getIf<int64_t>("id")) {
    return *i == expected;
  }
  if (const auto u = response.getIf<uint64_t>("id")) {
    return expected >= 0 && *u == static_cast<uint64_t>(expected);
  }
  if (const auto d = response.getIf<double>("id")) {
    return *d == static_cast<double>(expected);
  }
  return false;
}

} // namespace

Roe<Object> McpClient::ReadStdioResponse(const McpReadFn& read, const int64_t expected_id, std::string& carry) {
  size_t skipped = 0;
  char buffer[4096];
  while (true) {
    size_t newline = carry.find('\n');
    while (newline == std::string::npos) {
      if (carry.size() > kMaxMcpResponseBytes) {
        carry.clear();
        return Error("MCP response line too long (over " + std::to_string(kMaxMcpResponseBytes) + " bytes)");
      }
      const int n = read(buffer, sizeof(buffer));
      if (n == os::OsProcessPipe::kReadTimedOut) {
        carry.clear();
        return Error("MCP response timed out");
      }
      if (n == 0) {
        carry.clear();
        return Error("MCP process closed its output");
      }
      if (n < 0) {
        carry.clear();
        return Error("MCP read failed");
      }
      carry.append(buffer, static_cast<size_t>(n));
      newline = carry.find('\n');
    }
    std::string line = carry.substr(0, newline);
    carry.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty()) {
      auto value = ParseValue(line);
      const Object* response = value ? asObject(*value) : nullptr;
      if (!response) {
        carry.clear();
        return Error("MCP response parse failed");
      }
      if (IdMatches(*response, expected_id)) {
        return *response;
      }
    }
    if (++skipped > kMaxMcpSkippedLines) {
      carry.clear();
      return Error("MCP sent " + std::to_string(skipped) + " lines without the awaited response");
    }
  }
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
