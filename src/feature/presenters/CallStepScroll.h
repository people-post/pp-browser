#pragma once

namespace pbr {

/**
 * The answering page's step list is one list of rows (3 before the answer + the connection steps) in a clip of fixed
 * height. When the rows reached do not all fit, the list moves up a row at a time so the current row stays in sight.
 * The numbers below are the call page's rcss (assets/themes/components.rcss), asserted equal by a gui test.
 */
namespace call_page_dp {
/** A step row (22 dot) and the gap to the next one: the scroll step. */
inline constexpr int kStepRow = 34;
/** `.call-phone-top`: the bar with the chevron; the ring page has none and pads for it (`call-phone-story--ring`). */
inline constexpr int kTopBar = 44;
/** `.call-phone-story` padding-top. */
inline constexpr int kStoryPaddingTop = 59;
/** `.call-phone-pair` height. */
inline constexpr int kPairRow = 120;
/** `.call-phone-pair-names` margin-top, and the line of the other party's bold name. */
inline constexpr int kNamesMarginTop = 6;
inline constexpr int kPeerNameLine = 22;
/** `.call-phone-steps` margin-top. */
inline constexpr int kStepsMarginTop = 22;
/** Below the steps on the answering page: the button card (padding 12 + 52 circle + 4 + 13 label + 8) + margin 22. */
inline constexpr int kControlsReserve = 12 + 52 + 4 + 13 + 8 + 22;
/** From the top of the page (under the status bar) to the first step row. */
inline constexpr int kStepsStart =
    kTopBar + kStoryPaddingTop + kPairRow + kNamesMarginTop + kPeerNameLine + kStepsMarginTop;
} // namespace call_page_dp

/** Height of the step clip on the answering page; `page_height_dp` = window height less the status bar and bottom inset. */
int CallStepAreaHeightDp(int page_height_dp);

/**
 * Rows to move the list up. `reached_row` is the 1-based row now current (the row count when all are done). Rows that
 * fit: area / kStepRow; nothing moves while the reached row fits; unknown (<= 0) area moves nothing.
 */
int CallStepScrollRows(int reached_row, int area_height_dp);

} // namespace pbr
