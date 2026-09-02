#pragma once

#include "domain/ai/TurnPlan.h"

#include <chrono>
#include <string>
#include <vector>

namespace pbr {

struct TurnTrace {
  std::string turn_id;
  std::string entry_id;
  std::string thread_id;

  TurnPlanSource plan_source = TurnPlanSource::Planner;
  ResponseGoal response_goal = ResponseGoal::General;
  RenderMode render_mode = RenderMode::Blocks;

  std::vector<std::string> tools_planned;
  std::vector<std::string> tools_executed;

  bool refinement_used = false;
  bool output_repair_used = false;
  bool parse_ok = false;

  int64_t planner_ms = 0;
  int64_t synthesis_ms = 0;

  void Log() const;
};

inline int64_t ElapsedMs(const std::chrono::steady_clock::time_point& start) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
}

} // namespace pbr
