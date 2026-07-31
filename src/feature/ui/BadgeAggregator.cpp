#include "feature/ui/BadgeAggregator.h"

#include "feature/ui/ShellHost.h"

#include <string>

namespace pbr {

void BadgeAggregator::BindSource(std::function<BadgeUnreadInputs()> source) {
  source_ = std::move(source);
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
    next.sessions_unread_display = FormatBadgeCount(next.sessions_unread).c_str();
    next.contacts_unread_display = FormatBadgeCount(next.contacts_unread).c_str();
  }
  state_ = std::move(next);
  ShellHost::Instance().State().nav_badges = state_;
}

} // namespace pbr
