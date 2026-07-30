#pragma once

#include "base/ui/ShellTypes.h"

namespace pbr {

class MessagingHub;

class BadgeAggregator {
public:
  static BadgeAggregator& Instance();

  void BindMessaging(MessagingHub& messaging);
  MessagingHub& Hub();
  const MessagingHub& Hub() const;

  void Refresh();
  const NavBadgeState& State() const { return state_; }

private:
  BadgeAggregator() = default;

  NavBadgeState state_;
  MessagingHub* messaging_ = nullptr;
};

std::string FormatBadgeCount(int count);

} // namespace pbr
