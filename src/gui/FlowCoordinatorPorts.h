#pragma once

#include <functional>

namespace pbr {

/**
 * Modal flow ports for shell / people-picker.
 * Declared in UI (consumers); Application fills from owned FlowCoordinator.
 * Clear via BindFlowCoordinator({}).
 */
struct FlowCoordinatorPorts {
  std::function<void(int layer_id, std::function<bool()> on_step_back, std::function<void()> on_cancel)> begin_modal;
  std::function<void()> end_modal;
  std::function<bool()> handle_dismiss;
  std::function<void(int layer_id)> notify_layer_closing;
  /** A modal flow holds the one modal slot: a second modal panel is refused rather than taking Escape from it. */
  std::function<bool()> is_modal_active;
};

} // namespace pbr
