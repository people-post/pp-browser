#include "feature/presenters/CallStoryList.h"

#include "feature/presenters/CallStepScroll.h"

#include <algorithm>

namespace pbr {

CallStoryList PlanCallStoryList(const CallStoryListInput& in) {
  CallStoryList plan;
  plan.inbound = !in.view.outgoing;
  if (!in.story_active) {
    return plan;
  }
  const CallStorySteps steps = CallStoryLines(in.view, in.group);
  const int own = static_cast<int>(steps.keys.size());
  if (!in.view.outgoing && !in.group && !in.video_stage) {
    for (const CallRingStep& step : CallRingSteps(false, in.caller, in.media)) {
      plan.lead.push_back(step);
    }
  }
  const int lead = static_cast<int>(plan.lead.size());
  plan.reached_row = std::min(lead + own, lead + steps.shown + 1);
  if (lead == 0) {
    return plan; // only the answering page's list moves; the others are short and unclipped
  }
  using namespace call_page_dp;
  const int below = (in.hint_shown ? kHintLine : 0) + (in.debug_shown ? kDebugLine : 0);
  const int area = in.page_height_dp > 0 ? CallStepAreaHeightDp(in.page_height_dp) - below : 0;
  plan.scroll_rows = CallStepScrollRows(plan.reached_row, area);
  if (plan.scroll_rows > 0) {
    // Exactly the rows that fit (the last has no gap after it), so a hint under the list is not pushed away.
    plan.clip_dp = (area / kStepRow) * kStepRow - (kStepRow - 22);
  }
  return plan;
}

} // namespace pbr
