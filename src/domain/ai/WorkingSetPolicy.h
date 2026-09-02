#pragma once

#include "domain/ai/TurnPlan.h"
#include "common/ui/WorkingSetTypes.h"
#include "common/PbrCompat.h"

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

BlockEligibility EvaluateBlock(const Object& block, ResponseGoal goal);

std::string BuildWorkingSetTeaser(int block_index, const std::string& label);

ResponseGoal InferResponseGoalFromBlocksJson(const std::string& json);

} // namespace pbr
