#pragma once

#include "feature/conversations/MessagingShellPorts.h"
#include "gui/shell/StatusbarText.h"
#include "lib/ui/ShellTypes.h"

#include <functional>
#include "common/PbrCompat.h"

namespace ui {
class Context;
class DataModelConstructor;
}

namespace pbr {

/**
 * The desktop status bar: the relay / direct / inbound cluster and the load chips, and the popover
 * that details them (reachability, UPnP, network and relay rates), with Retest and Network settings.
 * Its fields are the window model's (ShellState statusbar_*); ShellHost owns it.
 *
 * UI thread.
 */
class ShellStatusbar {
public:
  ShellStatusbar(ShellState& state, ui::Context*& context, const MessagingShellPorts& ports,
                 std::function<void()> apply_safe_area_layout);

  void Bind(ui::DataModelConstructor& ctor);
  void Dirty();
  void RefreshVisibility();
  void OpenPopover();
  void ClosePopover();
  void TogglePopover();
  void RefreshCluster();

private:
  void DirtyPopover();
  void ClearCluster();
  bool ApplyCluster(const StatusbarClusterSnapshot& snap);
  void ClearPopover();
  bool ApplyPopover(const StatusbarPopoverSnapshot& snap);
  void RefreshPopover();
  void PositionPopover();
  void RetestReachability();
  void OpenNetworkSettings();

  ShellState& state_;
  ui::Context*& context_;
  const MessagingShellPorts& ports_;
  std::function<void()> apply_safe_area_layout_;
  bool popover_needs_position_ = false;
};

} // namespace pbr
