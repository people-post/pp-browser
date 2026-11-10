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
/** A hint line under the list (`.call-phone-hint`: margin 8 + line 20) and the diagnostics line (6 + 16). */
inline constexpr int kHintLine = 8 + 20;
inline constexpr int kDebugLine = 6 + 16;
/** From the top of the page (under the status bar) to the first step row. */
inline constexpr int kStepsStart =
    kTopBar + kStoryPaddingTop + kPairRow + kNamesMarginTop + kPeerNameLine + kStepsMarginTop;
/** The ring page (`.call-phone-hero--ring` for a group: 123 + 120 icon + name 20 + 34 + 4 + status 22 above the steps). */
inline constexpr int kGroupHeroEnd = 123 + 120 + 20 + 34 + 4 + 22;
/** Three rows of 22 and two gaps of 12. */
inline constexpr int kRingStepsBlock = kStepsMarginTop + 3 * 22 + 2 * 12;
/** Below the ring page's content: the answer bar (72 circle + 8 + label line(s) + margin 48), the paid ring's charge
 *  block (50 button + 40 padding; its disabled hint 8 gap + 8 margin + 2 lines) and a hint (8 margin + 2 lines of 20). */
inline constexpr int kAnswerBar = 72 + 8 + 48;
inline constexpr int kAnswerLabelLine = 18;
inline constexpr int kChargeBlock = 50 + 40;
inline constexpr int kChargeHint = 8 + 8 + 2 * 20;
inline constexpr int kRingHint = 8 + 2 * 20;
} // namespace call_page_dp

/** What is on the incoming-call page besides the pair / steps, for the rule below (all `false` / 0 for a plain ring). */
struct CallRingLayoutInput {
  /** Window height less the status bar and the bottom inset; 0: not known. */
  int page_height_dp = 0;
  bool group = false;
  bool conflict = false;
  bool guest = false;
  bool pricing = false;
  bool charge_enabled = false;
};

/**
 * Whether the ring page has room for its three step lines. They are the first thing to give way when the page is short
 * (an iPhone SE with a paid or conflict ring); the price, conflict and guest text never are. The estimate is
 * conservative: every hint takes two lines, and the answer labels of a conflict or paid ring two. Unknown height: show.
 */
bool CallRingShowsSteps(const CallRingLayoutInput& in);

/** Height of the step clip on the answering page; `page_height_dp` = window height less the status bar and bottom inset. */
int CallStepAreaHeightDp(int page_height_dp);

/**
 * Rows to move the list up. `reached_row` is the 1-based row now current (the row count when all are done). Rows that
 * fit: area / kStepRow; nothing moves while the reached row fits; unknown (<= 0) area moves nothing.
 */
int CallStepScrollRows(int reached_row, int area_height_dp);

} // namespace pbr
