#pragma once

#include "common/Module.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace pbr {

struct McpTool {
  std::string name;
  std::string description;
  nlohmann::json input_schema;
};

class McpClient : public Module {
public:
  McpClient();
  ~McpClient();

  McpClient(const McpClient&) = delete;
  McpClient& operator=(const McpClient&) = delete;

  bool Start(const std::string& command, const std::vector<std::string>& args = {});
  void Stop();
  bool IsRunning() const { return running_; }

  bool Initialize();
  std::vector<McpTool> ListTools();
  nlohmann::json CallTool(const std::string& name, const nlohmann::json& arguments);

  static McpClient& MockInstance();

private:
  nlohmann::json Request(const std::string& method, const nlohmann::json& params);

  bool running_ = false;
  bool mock_ = false;
  int request_id_ = 1;

#if defined(_WIN32)
  void* process_ = nullptr;
  void* stdin_write_ = nullptr;
  void* stdout_read_ = nullptr;
#else
  int child_pid_ = -1;
  int stdin_write_fd_ = -1;
  int stdout_read_fd_ = -1;
#endif
};

} // namespace pbr
