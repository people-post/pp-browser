#pragma once

#include "agent/LlmClient.h"
#include "agent/TurnPlan.h"
#include "agent/ToolRegistry.h"
#include "common/Error.h"
#include "contacts/ContactTypes.h"
#include "messaging/ThreadTypes.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

struct TurnExecutionResult {
  std::vector<ChatMessage> scratch_append;
  std::vector<std::string> tools_executed;
  std::optional<std::string> people_list_blocks;
  bool ok = true;
  std::string error;
};

class TurnExecutor {
public:
  using ToolActivityCallback = std::function<void(const std::string& tool_name, const std::string& status)>;

  static TurnExecutionResult Execute(const TurnPlan& plan, ToolRegistry& tools,
                                     const ToolActivityCallback& on_activity = {});
};

} // namespace pbr
