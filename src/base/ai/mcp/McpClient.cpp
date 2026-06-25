#include "base/ai/mcp/McpClient.h"

#include "base/net/HttpClient.h"

#include <sstream>

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

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
#ifndef _WIN32
  if (stdin_write_fd_ >= 0) {
    close(stdin_write_fd_);
    stdin_write_fd_ = -1;
  }
  if (stdout_read_fd_ >= 0) {
    close(stdout_read_fd_);
    stdout_read_fd_ = -1;
  }
  if (child_pid_ > 0) {
    kill(child_pid_, SIGTERM);
    waitpid(child_pid_, nullptr, 0);
    child_pid_ = -1;
  }
#endif
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
#ifndef _WIN32
  int stdin_pipe[2]{};
  int stdout_pipe[2]{};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
    log().error << "Failed to create pipes for MCP process";
    return false;
  }

  child_pid_ = fork();
  if (child_pid_ == 0) {
    dup2(stdin_pipe[0], STDIN_FILENO);
    dup2(stdout_pipe[1], STDOUT_FILENO);
    close(stdin_pipe[0]);
    close(stdin_pipe[1]);
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(command.c_str(), argv.data());
    _exit(127);
  }

  close(stdin_pipe[0]);
  close(stdout_pipe[1]);
  stdin_write_fd_ = stdin_pipe[1];
  stdout_read_fd_ = stdout_pipe[0];
  running_ = true;
  log().info << "Started MCP process: " << command << " (pid=" << child_pid_ << ")";
  return true;
#else
  (void)args;
  (void)command;
  mock_ = true;
  running_ = true;
  return true;
#endif
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
                                          {{"name", "search_people"},
                                           {"description", "Search people"},
                                           {"inputSchema", {{"type", "object"}}}},
                                          {{"name", "register_user"},
                                           {"description", "Register user"},
                                           {"inputSchema", {{"type", "object"}}}},
                                          {{"name", "relay_send"},
                                           {"description", "Relay send"},
                                           {"inputSchema", {{"type", "object"}}}},
                                          {{"name", "relay_poll_inbox"},
                                           {"description", "Relay poll"},
                                           {"inputSchema", {{"type", "object"}}}},
                                      })}};
    }
    if (method == "tools/call") {
      const auto name = params.value("name", "");
      if (name == "user_search") {
        return nlohmann::json{{"content",
                               nlohmann::json::array({{{"type", "text"},
                                                       {"text", R"([{"name":"Ada","email":"ada@example.com"}])"}}})}};
      }
      if (name == "search_people") {
        return nlohmann::json{{"content",
                               nlohmann::json::array({{{"type", "text"},
                                                       {"text", R"([{"hit_id":"hit_alice","display_name":"Alice","nickname":"alice","ids":[{"kind":"relay_user","value":"relay:alice123","primary":true}]}])"}}})}};
      }
      if (name == "register_user") {
        return nlohmann::json{
            {"content", nlohmann::json::array({{{"type", "text"}, {"text", R"({"success":true,"relay_user_id":"relay:test"})"}}})}};
      }
      if (name == "relay_send") {
        return nlohmann::json{{"content", nlohmann::json::array({{{"type", "text"}, {"text", R"({"success":true})"}}})}};
      }
      if (name == "relay_poll_inbox") {
        return nlohmann::json{{"content",
                               nlohmann::json::array({{{"type", "text"}, {"text", R"({"messages":[],"next_cursor":"0"})"}}})}};
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

#ifndef _WIN32
  nlohmann::json req = {{"jsonrpc", "2.0"},
                        {"id", request_id_++},
                        {"method", method},
                        {"params", params}};

  const std::string payload = req.dump() + "\n";
  log().debug << "MCP request: " << method;
  if (write(stdin_write_fd_, payload.data(), payload.size()) < 0) {
    log().error << "MCP write failed for method: " << method;
    return Error("MCP write failed");
  }

  std::string line;
  char ch = 0;
  while (read(stdout_read_fd_, &ch, 1) == 1) {
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
#else
  (void)method;
  (void)params;
  return nlohmann::json::object();
#endif
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
