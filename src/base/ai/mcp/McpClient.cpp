#include "base/ai/mcp/McpClient.h"

#include "base/net/HttpClient.h"
#include "base/platform/os/OsProcess.h"

namespace pbr {

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

Roe<nlohmann::json> McpClient::Request(const std::string& method, const nlohmann::json& params) {
  if (mock_) {
    if (method == "initialize") {
      return nlohmann::json{{"protocolVersion", "2024-11-05"}, {"capabilities", nlohmann::json::object()}};
    }
    if (method == "tools/list") {
      return nlohmann::json{{"tools", nlohmann::json::array({
                                          {{"name", "user_search"},
                                           {"description", "Search users by query"},
                                           {"inputSchema",
                                            {{"type", "object"},
                                             {"properties", {{"query", {{"type", "string"}}}}},
                                             {"required", nlohmann::json::array({"query"})}}}},
                                      })}};
    }
    if (method == "tools/call") {
      const auto name = params.value("name", "");
      if (name == "user_search") {
        return nlohmann::json{{"content",
                               nlohmann::json::array({{{"type", "text"},
                                                       {"text", R"([{"name":"Ada","email":"ada@example.com"}])"}}})}};
      }
    }
    return nlohmann::json::object();
  }

  if (http_) {
    nlohmann::json req = {{"jsonrpc", "2.0"},
                          {"id", request_id_++},
                          {"method", method},
                          {"params", params}};

    log().debug << "HTTP MCP request: " << method;
    auto response = HttpClient::Post(http_url_, req.dump(), {{"Content-Type", "application/json"}});
    if (!response) {
      log().error << "HTTP MCP request failed for " << method << ": " << response.error().message;
      return response.error();
    }
    if (response->status_code < 200 || response->status_code >= 300) {
      log().error << "HTTP MCP status " << response->status_code << " for " << method;
      return Error("HTTP MCP status " + std::to_string(response->status_code));
    }
    if (response->body.empty()) {
      return nlohmann::json::object();
    }

    auto resp = nlohmann::json::parse(response->body, nullptr, false);
    if (resp.is_discarded()) {
      log().error << "HTTP MCP response parse failed for method: " << method;
      return Error("MCP response parse failed");
    }
    if (resp.contains("error")) {
      log().error << "MCP error for " << method << ": " << resp["error"].dump();
      return Error(resp["error"].dump());
    }
    return resp.value("result", nlohmann::json::object());
  }

  if (!process_.IsActive()) {
    return nlohmann::json::object();
  }

  nlohmann::json req = {{"jsonrpc", "2.0"},
                        {"id", request_id_++},
                        {"method", method},
                        {"params", params}};

  const std::string payload = req.dump() + "\n";
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

  auto resp = nlohmann::json::parse(line, nullptr, false);
  if (resp.is_discarded()) {
    log().error << "MCP response parse failed for method: " << method;
    return Error("MCP response parse failed");
  }

  if (resp.contains("error")) {
    log().error << "MCP error for " << method << ": " << resp["error"].dump();
    return Error(resp["error"].dump());
  }
  return resp.value("result", nlohmann::json::object());
}

Roe<void> McpClient::Initialize() {
  auto result = Request("initialize", {{"protocolVersion", "2024-11-05"}, {"capabilities", nlohmann::json::object()}});
  if (!result) {
    log().error << "MCP initialize failed: " << result.error().message;
    return result.error();
  }
  log().info << "MCP initialized";
  return {};
}

Roe<std::vector<McpTool>> McpClient::ListTools() {
  auto result = Request("tools/list", nlohmann::json::object());
  if (!result) {
    return result.error();
  }

  std::vector<McpTool> tools;
  for (const auto& tool : result->value("tools", nlohmann::json::array())) {
    McpTool entry;
    entry.name = tool.value("name", "");
    entry.description = tool.value("description", "");
    entry.input_schema = tool.value("inputSchema", nlohmann::json::object());
    tools.push_back(std::move(entry));
  }
  return tools;
}

Roe<nlohmann::json> McpClient::CallTool(const std::string& name, const nlohmann::json& arguments) {
  return Request("tools/call", {{"name", name}, {"arguments", arguments}});
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
