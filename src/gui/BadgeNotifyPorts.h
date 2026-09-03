#pragma once

#include <functional>

namespace pbr {

/**
 * Nav badge notify ports for ChatController (no BadgeAggregator*).
 * Application fills from BadgeAggregator. Clear via BindBadgeNotify({}).
 */
struct BadgeNotifyPorts {
  std::function<void()> refresh;
  std::function<int()> sessions_unread;
};

} // namespace pbr
