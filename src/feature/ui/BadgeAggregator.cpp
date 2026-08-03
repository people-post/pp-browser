#include "feature/ui/BadgeAggregator.h"

#include <string>

namespace pbr {

void BadgeAggregator::BindSource(std::function<BadgeUnreadInputs()> source) {
  source_ = std::move(source);
}

void BadgeAggregator::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

std::string FormatBadgeCount(const int count) {
  if (count <= 0) {
    return "0";
  }
  if (count > 99) {
    return "99+";
  }
  return std::to_string(count);
}

void BadgeAggregator::Refresh() {
  NavBadgeState next;
  if (source_) {
    const BadgeUnreadInputs inputs = source_();
    next.sessions_unread = inputs.sessions_unread;
    next.contacts_unread = inputs.contacts_unread;
    next.me_attention = inputs.me_attention;
    next.sessions_unread_display = FormatBadgeCount(next.sessions_unread).c_str();
    next.contacts_unread_display = FormatBadgeCount(next.contacts_unread).c_str();
  }
  state_ = std::move(next);
  if (shell_navigation_.set_nav_badges) {
    shell_navigation_.set_nav_badges(state_);
  }
}

} // namespace pbr
