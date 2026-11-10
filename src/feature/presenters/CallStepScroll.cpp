#include "feature/presenters/CallStepScroll.h"

namespace pbr {

int CallStepAreaHeightDp(const int page_height_dp) {
  return page_height_dp - call_page_dp::kStepsStart - call_page_dp::kControlsReserve;
}

int CallStepScrollRows(const int reached_row, const int area_height_dp) {
  const int fit = area_height_dp / call_page_dp::kStepRow;
  if (area_height_dp <= 0 || fit <= 0 || reached_row <= fit) {
    return 0;
  }
  return reached_row - fit;
}

} // namespace pbr
