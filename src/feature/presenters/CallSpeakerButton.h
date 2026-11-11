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
 * What the speaker button shows: the tapped value while its route change runs, or while it waits at most
 * kSpeakerTapShownMs; otherwise the route the OS really has (`route_on`).
 */
inline bool CallSpeakerButtonOn(const std::optional<CallSpeakerTap>& tap, const IMediaView::AudioRouteChange state,
                                const int64_t now_ms, const bool route_on) {
  if (!tap) {
    return route_on;
  }
  if (state == IMediaView::AudioRouteChange::Running) {
    return tap->on;
  }
  if (state == IMediaView::AudioRouteChange::Queued && now_ms - tap->at_ms < kSpeakerTapShownMs) {
    return tap->on;
  }
  return route_on;
}

} // namespace pbr
