#pragma once

#include "lib/media/IMediaView.h"

#include <cstdint>
#include <optional>

namespace pbr {

/** A speaker tap whose route change the media engine runs later (Android: around the mic). */
struct CallSpeakerTap {
  bool on = false;
  int64_t at_ms = 0;
};

/** A tap whose route change has not started is shown at most this long, then the route as it is. */
inline constexpr int64_t kSpeakerTapShownMs = 1500;

/**
 * What the speaker button shows: the tapped value while its route change runs, while it waits behind the engine
 * reopening or rebuilding the audio devices (`reopening`: that can take seconds, and the button must not flicker
 * back), or while it waits at most kSpeakerTapShownMs; otherwise the route the OS really has (`route_on`).
 */
inline bool CallSpeakerButtonOn(const std::optional<CallSpeakerTap>& tap, const IMediaView::AudioRouteChange state,
                                const bool reopening, const int64_t now_ms, const bool route_on) {
  if (!tap) {
    return route_on;
  }
  if (state == IMediaView::AudioRouteChange::Running) {
    return tap->on;
  }
  if (state == IMediaView::AudioRouteChange::Queued && (reopening || now_ms - tap->at_ms < kSpeakerTapShownMs)) {
    return tap->on;
  }
  return route_on;
}

/** How a speaker tap changes the route (CallPresenter::ToggleSpeaker). */
struct CallSpeakerTapPlan {
  /** The media engine changes the route around the mic (closed first, reopened after): Android with media up. */
  bool around_the_mic = false;
  /** The route changes now, then the engine reopens its devices (the old order; developer check of the rebuild). */
  bool reopen_after = false;
};

/**
 * `needs_reopen`: the platform loses the mic on a route change and media is up (Android). `dev_old_order`: the
 * developer trigger asks for the old order this once. Elsewhere the route just changes.
 */
inline CallSpeakerTapPlan PlanCallSpeakerTap(const bool needs_reopen, const bool dev_old_order) {
  if (!needs_reopen) {
    return {};
  }
  return dev_old_order ? CallSpeakerTapPlan{.reopen_after = true} : CallSpeakerTapPlan{.around_the_mic = true};
}

} // namespace pbr
