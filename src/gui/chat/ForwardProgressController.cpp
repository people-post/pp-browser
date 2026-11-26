#include "gui/chat/ForwardProgressController.h"

#include "gui/shell/DataModelHost.h"

#include <ui/data/DataModelHandle.h>

#include <string>

namespace pbr {

bool ForwardProgressController::RegisterModel(ui::Context* context) {
  context_ = context;
  if (!context) {
    return false;
  }
  return DataModelHost::Instance().Register(context, "forward_progress", [this](ui::DataModelConstructor& ctor) {
    ctor.Bind("fp_title", &title_);
    ctor.Bind("fp_line", &line_);
    ctor.Bind("fp_bar_width", &bar_width_);
    ctor.Bind("fp_has_percent", &has_percent_);
    ctor.Bind("fp_finishing", &finishing_);
    ctor.Bind("fp_can_close", &can_close_);
    ctor.BindEventCallback("forward_progress_cancel", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      if (handlers_.cancel) {
        handlers_.cancel();
      }
    });
    ctor.BindEventCallback("forward_progress_close", [this](ui::DataModelHandle, ui::Event&, const ui::VariantList&) {
      if (handlers_.close) {
        handlers_.close();
      }
    });
  });
}

void ForwardProgressController::Show(const ForwardProgressView& view) {
  const ui::String title = view.title.c_str();
  const ui::String line = view.line.c_str();
  const bool has_percent = view.percent >= 0;
  const ui::String width = (std::to_string(view.percent < 0 ? 0 : view.percent) + "%").c_str();
  if (title == title_ && line == line_ && has_percent == has_percent_ && width == bar_width_ &&
      view.finishing == finishing_ && view.can_close == can_close_) {
    return; // drawn every tick: only a change is worth redrawing
  }
  title_ = title;
  line_ = line;
  has_percent_ = has_percent;
  bar_width_ = width;
  finishing_ = view.finishing;
  can_close_ = view.can_close;
  if (context_) {
    for (const char* field : {"fp_title", "fp_line", "fp_bar_width", "fp_has_percent", "fp_finishing", "fp_can_close"}) {
      DataModelHost::Instance().Dirty("forward_progress", field);
    }
  }
}

void ForwardProgressController::Open() {
  if (IsOpen() || !navigation_.push_layer) {
    return;
  }
  PaneSpec spec;
  spec.key = "forward_progress";
  spec.rml_path = "forward_progress";
  spec.role = PaneRole::Transient;
  spec.full_screen = true;
  layer_id_ = navigation_.push_layer(spec);
  if (!IsOpen()) {
    return;
  }
  if (flow_.begin_modal) {
    flow_.begin_modal(
        layer_id_,
        [this]() { return handlers_.back ? handlers_.back() : false; }, // the back key and Esc are Cancel, and are taken
        [this]() {
          layer_id_ = -1; // closed by someone else (a tapped notification)
          if (handlers_.closed) {
            handlers_.closed();
          }
        });
  }
}

void ForwardProgressController::Close() {
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
