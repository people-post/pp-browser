#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>

namespace pbr {

/** Unread totals for nav badges. Application computes from MessagingHub + shell. */
struct BadgeUnreadInputs {
  int sessions_unread = 0;
  /** Reserved for contacts-tab queues; always 0 until those exist. */
  int contacts_unread = 0;
};

class BadgeAggregator {
public:
  BadgeAggregator() = default;

  /** App fills from MessagingHub / ShellHost. Not a process singleton. Clear with BindSource({}). */
  void BindSource(std::function<BadgeUnreadInputs()> source);

  void Refresh();
  const NavBadgeState& State() const { return state_; }

private:
  NavBadgeState state_;
  std::function<BadgeUnreadInputs()> source_;
};

std::string FormatBadgeCount(int count);

} // namespace pbr
