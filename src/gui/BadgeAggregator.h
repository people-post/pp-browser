#pragma once

#include "domain/ui/ShellTypes.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <functional>

namespace pbr {

/** Unread totals for nav badges. Application computes from ConversationsHub + shell. */
struct BadgeUnreadInputs {
  int sessions_unread = 0;
  /** Reserved for contacts-tab queues; fed from ContactsShellBridge when present. */
  int contacts_unread = 0;
  /** Me-tab attention (reachability nudge, etc.). */
  bool me_attention = false;
};

class BadgeAggregator {
public:
  BadgeAggregator() = default;

  /** App fills from ConversationsHub / ShellHost. Not a process singleton. Clear with BindSource({}). */
  void BindSource(std::function<BadgeUnreadInputs()> source);
  /** Nav badge write port without ShellHost::Instance(). Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);

  void Refresh();
  const NavBadgeState& State() const { return state_; }

private:
  NavBadgeState state_;
  std::function<BadgeUnreadInputs()> source_;
  ShellNavigationPorts shell_navigation_;
};

std::string FormatBadgeCount(int count);

} // namespace pbr
