#include "feature/ui/FlowCoordinator.h"

namespace pbr {

void FlowCoordinator::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void FlowCoordinator::BeginModal(int layer_id, StepBackHandler on_step_back, CancelHandler on_cancel) {
  layer_id_ = layer_id;
  on_step_back_ = std::move(on_step_back);
  on_cancel_ = std::move(on_cancel);
}

void FlowCoordinator::EndModal() {
  layer_id_ = -1;
  on_step_back_ = {};
  on_cancel_ = {};
}

bool FlowCoordinator::IsActive() const {
  return layer_id_ >= 0;
}

bool FlowCoordinator::HandleDismiss() {
  if (layer_id_ < 0) {
    return false;
  }
  if (on_step_back_ && on_step_back_()) {
    return true;
  }
  const int closing_id = layer_id_;
  CancelHandler cancel = std::move(on_cancel_);
  EndModal();
  if (cancel) {
    cancel();
  }
  if (closing_id >= 0 && shell_navigation_.close_layer) {
    shell_navigation_.close_layer(closing_id);
  }
  return true;
}

void FlowCoordinator::NotifyLayerClosing(int layer_id) {
  if (layer_id_ < 0 || layer_id_ != layer_id) {
    return;
  }
  CancelHandler cancel = std::move(on_cancel_);
  EndModal();
  if (cancel) {
    cancel();
  }
}

} // namespace pbr
