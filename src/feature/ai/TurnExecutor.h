#pragma once

#include "base/ai/LlmClient.h"
#include "base/ai/TurnPlan.h"
#include "feature/ai/ToolRegistry.h"
#include "common/Error.h"
#include "base/people/ContactTypes.h"
#include "base/messaging/ThreadTypes.h"

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
