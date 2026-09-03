#pragma once

#include "gui/shell/ShellNavigationPorts.h"

#include <functional>

namespace pbr {

/** Coordinates multi-step modal flows over ShellHost overlay layers. */
class FlowCoordinator {
public:
  FlowCoordinator() = default;

  using StepBackHandler = std::function<bool()>;
  using CancelHandler = std::function<void()>;

  /** Layer close without ShellHost::Instance(). Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);

  /** Begin a modal flow tied to an overlay layer. */
  void BeginModal(int layer_id, StepBackHandler on_step_back, CancelHandler on_cancel);

  void EndModal();

  bool IsActive() const;

  /** Handle Escape/back for an in-progress modal flow. Returns true when consumed. */
  bool HandleDismiss();

  /** Called when an overlay layer closes (scrim, close button, or programmatic). */
  void NotifyLayerClosing(int layer_id);

private:
  int layer_id_ = -1;
  StepBackHandler on_step_back_;
  CancelHandler on_cancel_;
  ShellNavigationPorts shell_navigation_;
};

} // namespace pbr
