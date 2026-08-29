#include "base/ai/TurnTrace.h"

#include "common/Logger.h"

#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

void TurnTrace::Log() const {
  std::ostringstream out;
  out << "turn_id=" << turn_id;
  if (!entry_id.empty()) {
    out << " entry_id=" << entry_id;
  }
  if (!thread_id.empty()) {
    out << " thread_id=" << thread_id;
  }
  out << " plan_source=" << TurnPlanSourceName(plan_source);
  out << " response_goal=" << ResponseGoalName(response_goal);
  out << " render_mode=" << RenderModeName(render_mode);
  out << " tools_planned=[";
  for (size_t i = 0; i < tools_planned.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << tools_planned[i];
  }
  out << "] tools_executed=[";
  for (size_t i = 0; i < tools_executed.size(); ++i) {
    if (i > 0) {
      out << ',';
    }
    out << tools_executed[i];
  }
  out << "]";
  out << " refinement_used=" << (refinement_used ? "true" : "false");
  out << " output_repair_used=" << (output_repair_used ? "true" : "false");
  out << " parse_ok=" << (parse_ok ? "true" : "false");
  out << " planner_ms=" << planner_ms;
  out << " synthesis_ms=" << synthesis_ms;

  logging::getLogger("TurnTrace").info << out.str();
}

} // namespace pbr
