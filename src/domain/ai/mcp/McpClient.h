#pragma once

#include "foundation/platform/os/OsProcess.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/net/HttpTransport.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** MCP tool `annotations` (spec: tools may omit these; absence must not be read as "safe"). */
struct McpToolAnnotations {
  std::optional<bool> read_only_hint;
  std::optional<bool> destructive_hint;
};

// Limits of the line-framed stdio transport (PB-056).
inline constexpr size_t kMaxMcpResponseBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxMcpSkippedLines = 64;
inline constexpr int kMcpReadTimeoutMs = 30'000;

/** Reads up to `size` bytes: count read, 0 at EOF, -1 on error, os::OsProcessPipe::kReadTimedOut on timeout. */
using McpReadFn = std::function<int(char*, size_t)>;

struct McpTool {
  std::string name;
  std::string description;
  Object input_schema;
  McpToolAnnotations annotations;
};

class McpClient : public Module {
public:
  McpClient();
  ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  bool Start(const std::string& command, const std::vector<std::string>& args = {});
  bool StartHttp(const std::string& url);
  void Stop();
  bool IsRunning() const { return running_; }

  /** Feature/app wires HttpClient::Post so mcp does not include base/net. */
  static void SetHttpPost(HttpPostFn post);

  Roe<void> Initialize();
  Roe<std::vector<McpTool>> ListTools();
  Roe<Object> CallTool(const std::string& name, const Object& arguments);

  static McpClient& MockInstance();

  /**
   * One JSON-RPC message from a line-framed stdio stream: the line whose `id` equals `expected_id`, as the whole
   * response object. Notifications and other lines are skipped, at most kMaxMcpSkippedLines. A line over
   * kMaxMcpResponseBytes, EOF, a read error or a timeout is an error, and `carry` is dropped. Bytes after the
   * response stay in `carry` for the next call.
   */
  static Roe<Object> ReadStdioResponse(const McpReadFn& read, int64_t expected_id, std::string& carry);

private:
  Roe<Object> Request(const std::string& method, const Object& params);

  bool running_ = false;
  bool mock_ = false;
  bool http_ = false;
  std::string http_url_;
  int request_id_ = 1;
  os::OsProcessPipe process_;
  std::string stdio_carry_;
  static HttpPostFn& HttpPostSlot();
};

} // namespace pbr
