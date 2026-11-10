#include "feature/presenters/CallStepScroll.h"

namespace pbr {

int CallStepAreaHeightDp(const int page_height_dp) {
  return page_height_dp - call_page_dp::kStepsStart - call_page_dp::kControlsReserve;
}

bool CallRingShowsSteps(const CallRingLayoutInput& in) {
  using namespace call_page_dp;
  if (in.page_height_dp <= 0) {
    return true;
  }
  const int label_lines = in.conflict || in.pricing ? 2 : 1;
  // Down to the end of the steps: what is above them (the pair and names / the group hero), their margin and rows.
  int needed = (in.group ? kGroupHeroEnd : kStepsStart - kStepsMarginTop) + kRingStepsBlock;
  needed += kAnswerBar + label_lines * kAnswerLabelLine;
  needed += (in.conflict ? kRingHint : 0) + (in.guest ? kRingHint : 0) + (in.pricing ? kRingHint : 0);
  if (in.pricing) {
    needed += kChargeBlock + (in.charge_enabled ? 0 : kChargeHint);
  }
  return needed <= in.page_height_dp;
}

int CallStepScrollRows(const int reached_row, const int area_height_dp) {
  const int fit = area_height_dp / call_page_dp::kStepRow;
  if (area_height_dp <= 0 || fit <= 0 || reached_row <= fit) {
    return 0;
  }
  return reached_row - fit;
}

} // namespace pbr
