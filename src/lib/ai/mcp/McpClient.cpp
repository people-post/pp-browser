#include "lib/ai/mcp/McpClient.h"

#include "foundation/platform/os/OsProcess.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <mutex>

namespace pbr {

namespace {

/** Every agent session sets the slot when it configures (on a worker, several at once) while others post through it. */
std::mutex& HttpPostMutex() {
  static std::mutex mutex;
  return mutex;
}

} // namespace

HttpPostFn& McpClient::HttpPostSlot() {
  static HttpPostFn post;
  return post;
}

void McpClient::SetHttpPost(HttpPostFn post) {
  std::lock_guard lock(HttpPostMutex());
  HttpPostSlot() = std::move(post);
}

void McpClient::SetHttpPostIfUnset(HttpPostFn post) {
  std::lock_guard lock(HttpPostMutex());
  if (!HttpPostSlot()) {
    HttpPostSlot() = std::move(post);
  }
}

namespace {

Object EmptyObject() {
  return {};
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
  http_ = false;
  http_url_.clear();
}

bool McpClient::Start(const std::string& command, const std::vector<std::string>& args) {
  Stop();
  if (!process_.Start(command, args)) {
    // A server that cannot start is a failure the user sees (PB-056).
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
  if (http_) {
    int64_t id = 0;
    {
      std::lock_guard lock(request_mutex_);
      id = request_id_++;
    }
    answered_ = false;
    Object req;
    req.set("jsonrpc", "2.0");
    req.set("id", id);
    req.set("method", method);
    req.set("params", params);

    log().debug << "HTTP MCP request: " << method;
    HttpPostFn post;
    {
      std::lock_guard lock(HttpPostMutex());
      post = HttpPostSlot();
    }
    if (!post) {
      return Error("MCP HTTP transport is not configured");
    }
    auto response = post(http_url_, DumpJson(req), {{"Content-Type", "application/json"}}, timeout_s);
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

  std::lock_guard lock(request_mutex_); // one exchange at a time on the pipe
  answered_ = false;
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

Roe<void> McpClient::Initialize(const long timeout_s) {
  Object params;
  params.set("protocolVersion", "2024-11-05");
  params.set("capabilities", EmptyObject());
  auto result = Request("initialize", params, timeout_s);
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

} // namespace pbr
