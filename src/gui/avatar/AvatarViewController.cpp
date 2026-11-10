#include "gui/avatar/AvatarViewController.h"

#include "gui/shell/DataModelHost.h"

#include <ui/data/DataModelHandle.h>

namespace pbr {

void AvatarViewController::BindShellNavigation(ShellNavigationPorts ports) {
  navigation_ = std::move(ports);
}
void AvatarViewController::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_ = std::move(ports);
}

bool AvatarViewController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "avatar_view", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("avatar_view_src", &src_);
    ctor.BindEventCallback("avatar_view_close",
                           [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) { Close(); });
  });
}

void AvatarViewController::Open(const std::string& image_src) {
  if (image_src.empty() || IsOpen() || !navigation_.push_layer) {
    return;
  }
  src_ = image_src.c_str();
  if (context_) {
    DataModelHost::Instance().Dirty("avatar_view", "avatar_view_src");
  }
  PaneSpec spec;
  spec.key = "avatar_view";
  spec.rml_path = "avatar_view";
  spec.role = PaneRole::Transient;
  spec.full_screen = true;
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
    return;
  }
  if (flow_.begin_modal) {
    // The back key and a swipe cancel through the flow coordinator; nothing is in flight, so there is no step back.
    flow_.begin_modal(layer_id_, []() { return false; }, [this]() { layer_id_ = -1; });
  }
}

void AvatarViewController::Close() {
  const int layer = layer_id_;
  layer_id_ = -1;
  if (layer < 0) {
    return;
  }
  if (flow_.end_modal) {
    flow_.end_modal();
  }
  if (navigation_.close_layer) {
    navigation_.close_layer(layer);
  }
}

} // namespace pbr
