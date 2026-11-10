#include "gui/BadgeAggregator.h"

#include <string>

namespace pbr {

void BadgeAggregator::BindSource(std::function<BadgeUnreadInputs()> source) {
  source_ = std::move(source);
}

void BadgeAggregator::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void BadgeAggregator::BindAppBadge(std::function<void(int)> set_app_badge) {
  set_app_badge_ = std::move(set_app_badge);
}

void BadgeAggregator::ClearAppBadge() {
  if (set_app_badge_) {
    set_app_badge_(0);
  }
  last_sent_ = 0;
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
    if (inputs.app_icon_count && last_sent_ != inputs.app_icon_count) {
      last_sent_ = inputs.app_icon_count;
      if (set_app_badge_) {
        set_app_badge_(*inputs.app_icon_count);
      }
    }
  }
  state_ = std::move(next);
  if (shell_navigation_.set_nav_badges) {
    shell_navigation_.set_nav_badges(state_);
  }
}

} // namespace pbr
