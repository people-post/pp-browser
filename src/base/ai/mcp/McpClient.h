#pragma once

#include "foundation/platform/os/OsProcess.h"
#include "common/Error.h"
#include "common/Module.h"
#include "common/net/HttpTransport.h"
#include "common/PbrCompat.h"

#include <string>
#include <vector>

namespace pbr {

struct McpTool {
  std::string name;
  std::string description;
  Object input_schema;
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

private:
  Roe<Object> Request(const std::string& method, const Object& params);

  bool running_ = false;
  bool mock_ = false;
  bool http_ = false;
  std::string http_url_;
  int request_id_ = 1;
  os::OsProcessPipe process_;
  static HttpPostFn& HttpPostSlot();
};

} // namespace pbr
