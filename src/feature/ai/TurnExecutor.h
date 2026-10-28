#pragma once

#include "lib/ai/LlmClient.h"
#include "lib/ai/TurnPlan.h"
#include "lib/ai/ToolPermissionPolicy.h"
#include "lib/ai/ToolRegistry.h"
#include "common/Error.h"
#include "lib/people/ContactTypes.h"
#include "common/thread/ThreadTypes.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

struct TurnExecutionResult {
  std::vector<ChatMessage> scratch_append;
  std::vector<std::string> tools_executed;
  /** Each executed tool's raw result, in order (local plans render from these). */
  std::vector<std::string> raw_results;
  /** Deterministic blocks JSON (people list, or the article feed of a payload turn): the turn ends without a model call. */
  std::optional<std::string> people_list_blocks;
  bool ok = true;
  std::string error;
  /** Stopped before a mutating tool that needs user permission. */
  bool needs_permission = false;
  size_t next_tool_index = 0;
  std::vector<PlannedToolCall> offered_tools;
};

struct TurnExecutionOptions {
  size_t start_index = 0;
  ToolPermissionsPrefs permissions;
  std::unordered_set<std::string> session_grants;
  /** When true, Ask is treated as Deny with a tool error (refinement loop). */
  bool deny_on_ask = false;
};

class TurnExecutor {
public:
  using ToolActivityCallback = std::function<void(const std::string& tool_name, const std::string& status)>;

  static TurnExecutionResult Execute(const TurnPlan& plan, ToolRegistry& tools,
                                     const ToolActivityCallback& on_activity = {},
                                     const TurnExecutionOptions& options = {});
};

} // namespace pbr
