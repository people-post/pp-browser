#pragma once

#include "agent/TurnPlan.h"
#include "ui/WorkingSetTypes.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace pbr {

struct WorkingSetRouting {
  bool panel_primary = false;
  bool auto_open_eligible = true;
};

struct BlockEligibility {
  bool eligible = false;
  WorkingSetKind kind = WorkingSetKind::None;
  WorkingSetAffinity affinity = WorkingSetAffinity::None;
  bool auto_open = false;
  bool promote_to_panel_only = false;
  std::string title;
  std::string subtitle;
  std::string teaser_label;
};

WorkingSetRouting RouteTurn(ResponseGoal goal, RenderMode render_mode);

BlockEligibility EvaluateBlock(const nlohmann::json& block, ResponseGoal goal);

std::string BuildWorkingSetTeaser(int block_index, const std::string& label);

ResponseGoal InferResponseGoalFromBlocksJson(const std::string& json);

} // namespace pbr
