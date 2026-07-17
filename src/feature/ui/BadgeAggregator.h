#pragma once

#include "base/ui/ShellTypes.h"

namespace pbr {

class BadgeAggregator {
public:
  static BadgeAggregator& Instance();

  void Refresh();
  const NavBadgeState& State() const { return state_; }

private:
  BadgeAggregator() = default;

  NavBadgeState state_;
};

std::string FormatBadgeCount(int count);

} // namespace pbr
