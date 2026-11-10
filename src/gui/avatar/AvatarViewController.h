#pragma once

#include "gui/FlowCoordinatorPorts.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <ui/base/Types.h>

#include <string>

namespace ui {
class Context;
}

namespace pbr {

/**
 * The profile photo shown large: a full-screen dark layer with the stored square centred, dismissed by a tap anywhere,
 * the close button or the back key. Pure display: it holds nothing but the picture's address. UI thread.
 */
class AvatarViewController {
public:
  void BindShellNavigation(ShellNavigationPorts ports);
  void BindFlowCoordinator(FlowCoordinatorPorts ports);
  bool RegisterModel(ui::Context* context);

  /** Opens the layer over the picture at `image_src` (the profile's icon file); empty does nothing. */
  void Open(const std::string& image_src);
  void Close();
  bool IsOpen() const { return layer_id_ >= 0; }
  void Shutdown() { Close(); }

private:
  ShellNavigationPorts navigation_;
  FlowCoordinatorPorts flow_;
  ui::Context* context_ = nullptr;
  int layer_id_ = -1;
  ui::String src_;
};

} // namespace pbr
