#pragma once

namespace pbr {

/**
 * Phone bars that hide on scroll (Kenneth 2026-11-05, PLAN-ui-refresh-impl 原话 78, 80; iOS hide-on-swipe): the chat
 * header (direct and group) and a pushed page's bar (settings pages, the contact / member page, the blocked list)
 * slide up out of view once the user scrolls away from the page's home position (a chat's newest message, a page's
 * top) and slide back when they scroll back towards it or reach it. The decisions, without any DOM.
 *
 * The distance is measured from home, which neither a page of older history loaded on top of a chat nor a new message
 * while pinned changes: moving kHideDp away from where the user turned hides the bar, moving kShowDp back, or coming
 * within kHomeEndDp of home, shows it. In between it keeps its state (no flapping while the finger wobbles).
 */
struct NavBarCollapseModel {
  /** This close to home the bar always shows (and a page's bar is clear: nothing is under it yet). */
  static constexpr float kHomeEndDp = 8.f;
  /** Scrolled this far away from home from where the user turned, the bar hides... */
  static constexpr float kHideDp = 24.f;
  /** ...and back this far towards home, it shows again. */
  static constexpr float kShowDp = 32.f;

  /** A page's bar takes its material once content has scrolled under it; at home it is clear. */
  static bool ContentUnderBar(const float from_home_dp) { return from_home_dp > kHomeEndDp; }

  /** One bar. A new page or chat starts shown (it opens at home). */
  struct Bar {
    bool hidden = false;
    /** Where the current run started: the nearest point to home while shown, the furthest while hidden. */
    float turn_dp = 0.f;

    /** `from_home_dp`: how far the content is scrolled away from home. True when `hidden` flipped. */
    bool OnScroll(const float from_home_dp) {
      const bool was = hidden;
      if (from_home_dp <= kHomeEndDp) {
        hidden = false;
        turn_dp = from_home_dp;
      } else if (!hidden) {
        if (from_home_dp < turn_dp) {
          turn_dp = from_home_dp;
        } else if (from_home_dp - turn_dp >= kHideDp) {
          hidden = true;
          turn_dp = from_home_dp;
        }
      } else if (from_home_dp > turn_dp) {
        turn_dp = from_home_dp;
      } else if (turn_dp - from_home_dp >= kShowDp) {
        hidden = false;
        turn_dp = from_home_dp;
      }
      return hidden != was;
    }

    void Reset() { *this = Bar{}; }
  };
};

} // namespace pbr
