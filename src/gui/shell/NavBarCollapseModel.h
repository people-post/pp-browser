#pragma once

namespace pbr {

/**
 * Phone collapsing bars (Kenneth 2026-11-04, PLAN-ui-refresh-impl 原话 71): the decisions without any DOM.
 *
 * The chat header is taller at rest (avatar, name, subtitle) and shrinks to a thin bar with a centred name while the
 * user reads older history, so more of the transcript shows. It goes by the distance from the newest message (the
 * bottom of the transcript), which neither a page of older history loaded on top nor a new message while pinned
 * changes: moving that far into older history collapses it, moving back towards the newest that far, or reaching the
 * newest end, expands it again. In between it keeps its state (no flapping while the finger wobbles).
 *
 * Sizes here are the ones components.rcss gives .shell-chat-overlay-chrome and its .chat-bar-compact state.
 */
struct NavBarCollapseModel {
  /** The chat header's bar at rest and collapsed (components.rcss; the collapse is a transform, not a layout). */
  static constexpr float kChatBarDp = 56.f;
  static constexpr float kChatBarCompactDp = 44.f;
  /** This close to the newest message the header is always expanded. */
  static constexpr float kChatNewestEndDp = 8.f;
  /** Scrolled this far into older history from where the user turned, the header collapses... */
  static constexpr float kChatCollapseDp = 24.f;
  /** ...and back this far towards the newest, it expands. */
  static constexpr float kChatExpandDp = 32.f;

  /** One open chat's header. A new chat starts expanded (it opens at its newest message). */
  struct ChatHeader {
    bool compact = false;
    /** Where the current run started: the nearest point to the newest end while expanded, the furthest while compact. */
    float turn_dp = 0.f;

    /** `from_newest_dp`: how far the transcript is scrolled up from its newest end. True when `compact` flipped. */
    bool OnScroll(const float from_newest_dp) {
      const bool was = compact;
      if (from_newest_dp <= kChatNewestEndDp) {
        compact = false;
        turn_dp = from_newest_dp;
      } else if (!compact) {
        if (from_newest_dp < turn_dp) {
          turn_dp = from_newest_dp;
        } else if (from_newest_dp - turn_dp >= kChatCollapseDp) {
          compact = true;
          turn_dp = from_newest_dp;
        }
      } else if (from_newest_dp > turn_dp) {
        turn_dp = from_newest_dp;
      } else if (turn_dp - from_newest_dp >= kChatExpandDp) {
        compact = false;
        turn_dp = from_newest_dp;
      }
      return compact != was;
    }

    void Reset() { *this = ChatHeader{}; }
  };
};

} // namespace pbr
