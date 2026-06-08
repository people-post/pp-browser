#include "mcp/McpClient.h"

#include <sstream>
#include <stdexcept>

#ifndef _WIN32
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace ppbrowser {

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
}

bool McpClient::Start(const std::string& command, const std::vector<std::string>& args) {
  Stop();
  if (command == "mock") {
    mock_ = true;
    running_ = true;
    return true;
  }
#ifndef _WIN32
  int stdin_pipe[2]{};
  int stdout_pipe[2]{};
  if (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0) {
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
  return true;
#else
  (void)args;
  (void)command;
  mock_ = true;
  running_ = true;
  return true;
#endif
}

nlohmann::json McpClient::Request(const std::string& method, const nlohmann::json& params) {
  if (mock_) {
    if (method == "initialize") {
      return {{"protocolVersion", "2024-11-05"}, {"capabilities", nlohmann::json::object()}};
    }
    if (method == "tools/list") {
      return {{"tools", nlohmann::json::array({
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
        return {{"content",
                 nlohmann::json::array({{{"type", "text"},
                                         {"text", R"([{"name":"Ada","email":"ada@example.com"}])"}}})}};
      }
    }
    return nlohmann::json::object();
  }

#ifndef _WIN32
  nlohmann::json req = {{"jsonrpc", "2.0"},
                        {"id", request_id_++},
                        {"method", method},
                        {"params", params}};

  const std::string payload = req.dump() + "\n";
  if (write(stdin_write_fd_, payload.data(), payload.size()) < 0) {
    throw std::runtime_error("MCP write failed");
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
    throw std::runtime_error("MCP empty response");
  }
  auto resp = nlohmann::json::parse(line);
  if (resp.contains("error")) {
    throw std::runtime_error(resp["error"].dump());
  }
  return resp.value("result", nlohmann::json::object());
#else
  (void)method;
  (void)params;
  return nlohmann::json::object();
#endif
}

bool McpClient::Initialize() {
  try {
    Request("initialize", {{"protocolVersion", "2024-11-05"}, {"capabilities", nlohmann::json::object()}});
    return true;
  } catch (...) {
    return false;
  }
}

std::vector<McpTool> McpClient::ListTools() {
  std::vector<McpTool> tools;
  auto result = Request("tools/list", nlohmann::json::object());
  for (const auto& tool : result.value("tools", nlohmann::json::array())) {
    McpTool entry;
    entry.name = tool.value("name", "");
    entry.description = tool.value("description", "");
    entry.input_schema = tool.value("inputSchema", nlohmann::json::object());
    tools.push_back(std::move(entry));
  }
  return tools;
}

nlohmann::json McpClient::CallTool(const std::string& name, const nlohmann::json& arguments) {
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

} // namespace ppbrowser
