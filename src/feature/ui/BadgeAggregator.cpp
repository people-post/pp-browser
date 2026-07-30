#include <stdexcept>
#include "feature/ui/BadgeAggregator.h"

#include "feature/messaging/MessagingHub.h"
#include "feature/ui/ShellHost.h"

#include <algorithm>
#include <string>

namespace pbr {

namespace {

int ActiveThreadUnreadDeduction(const InboxController& inbox, const NavTab tab) {
  if (tab != NavTab::Sessions) {
    return 0;
  }
  const std::string& active_id = inbox.ActiveThreadId();
  if (active_id.empty() || !BadgeAggregator::Instance().Hub().IsInitialized()) {
    return 0;
  }
  auto thread = BadgeAggregator::Instance().Hub().Store().GetThread(active_id);
  if (!thread || !*thread) {
    return 0;
  }
  return (*thread)->unread_count;
}

} // namespace

BadgeAggregator& BadgeAggregator::Instance() {
  static BadgeAggregator aggregator;
  return aggregator;
}
void BadgeAggregator::BindMessaging(MessagingHub& messaging) {
  messaging_ = &messaging;
}

MessagingHub& BadgeAggregator::Hub() {
  if (!messaging_) {
    throw std::runtime_error("BadgeAggregator messaging not bound");
  }
  return *messaging_;
}

const MessagingHub& BadgeAggregator::Hub() const {
  if (!messaging_) {
    throw std::runtime_error("BadgeAggregator messaging not bound");
  }
  return *messaging_;
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
  if (Hub().IsInitialized()) {
    auto& inbox = Hub().Inbox();
    const int total = inbox.SumUnread();
    const NavTab tab = ShellHost::Instance().State().nav_tab;
    const int deduction = ActiveThreadUnreadDeduction(inbox, tab);
    // Sessions owns aggregate chat unread. Contacts nav stays at 0 until a
    // contacts-tab queue exists (intro requests, pending invites, etc.).
    next.sessions_unread = std::max(0, total - deduction);
    next.contacts_unread = 0;
    next.sessions_unread_display = FormatBadgeCount(next.sessions_unread).c_str();
    next.contacts_unread_display = FormatBadgeCount(next.contacts_unread).c_str();
  }
  state_ = std::move(next);
  ShellHost::Instance().State().nav_badges = state_;
}

} // namespace pbr
