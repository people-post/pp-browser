#include "feature/ui/ShellBottomSheetGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr float kDismissThresholdRatio = 0.25f;
constexpr int kDragDeadzonePx = 8;

int EventMouseY(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

} // namespace

void ShellBottomSheetGesture::Attach(Rml::Element* sheet, Rml::Context* context, float sheet_height_dp,
                                     DismissCallback on_dismiss) {
  Detach();
  if (!sheet || !context) {
    return;
  }
  sheet_ = sheet;
  document_ = sheet_->GetOwnerDocument();
  context_ = context;
  sheet_height_dp_ = sheet_height_dp;
  on_dismiss_ = std::move(on_dismiss);
  sheet_->AddEventListener(Rml::EventId::Mousedown, this);
  attached_ = true;
  SetSheetOffset(0.f, false);
}

void ShellBottomSheetGesture::Detach() {
  SetDocumentDragCapture(false);
  if (attached_ && sheet_) {
    sheet_->RemoveEventListener(Rml::EventId::Mousedown, this);
  }
  sheet_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  attached_ = false;
  tracking_ = false;
  dragging_ = false;
}

void ShellBottomSheetGesture::SetDocumentDragCapture(bool enabled) {
  if (!document_ || document_drag_capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(Rml::EventId::Mousemove, this, true);
    document_->AddEventListener(Rml::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(Rml::EventId::Mousemove, this, true);
    document_->RemoveEventListener(Rml::EventId::Mouseup, this, true);
  }
  document_drag_capture_ = enabled;
}

float ShellBottomSheetGesture::PixelDeltaToDp(int delta_px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  if (ratio <= 0.f) {
    return static_cast<float>(delta_px);
  }
  return static_cast<float>(delta_px) / ratio;
}

float ShellBottomSheetGesture::ResolveSheetHeightDp() const {
  if (sheet_ && context_) {
    const float ratio = context_->GetDensityIndependentPixelRatio();
    const float height_px = sheet_->GetBox().GetSize(Rml::BoxArea::Border).y;
    if (ratio > 0.f && height_px > 0.f) {
      return height_px / ratio;
    }
  }
  return sheet_height_dp_;
}

bool ShellBottomSheetGesture::ShouldIgnoreTarget(Rml::Element* target) const {
  if (!target) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select" || tag == "button") {
      return true;
    }
  }
  return false;
}

bool ShellBottomSheetGesture::ShouldStartSwipe(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node->IsClassSet("shell-account-sheet-grabber") || node->IsClassSet("shell-account-sheet-header")) {
      return true;
    }
  }
  return false;
}

void ShellBottomSheetGesture::SetSheetOffset(float dy_dp, bool animate) {
  if (!sheet_) {
    return;
  }
  sheet_->SetClass("shell-account-sheet--dragging", !animate);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateY(%.1fdp)", dy_dp < 0.f ? 0.f : dy_dp);
  sheet_->SetProperty("transform", buffer);
}

void ShellBottomSheetGesture::BeginDrag(int y_px) {
  tracking_ = true;
  dragging_ = false;
  drag_start_y_px_ = y_px;
  drag_last_y_px_ = y_px;
  SetDocumentDragCapture(true);
}

void ShellBottomSheetGesture::UpdateDrag(int y_px, Rml::Event& event) {
  if (!tracking_ || !sheet_) {
    return;
  }
  drag_last_y_px_ = y_px;
  const int dy_px = y_px - drag_start_y_px_;
  if (!dragging_) {
    if (dy_px > kDragDeadzonePx) {
      dragging_ = true;
      sheet_->SetClass("shell-account-sheet--dragging", true);
    } else {
      return;
    }
  }
  const float dy_dp = PixelDeltaToDp(dy_px);
  if (dy_dp > 0.f) {
    SetSheetOffset(dy_dp, false);
    event.StopPropagation();
  } else {
    SetSheetOffset(0.f, false);
  }
}

void ShellBottomSheetGesture::EndDrag() {
  if (!tracking_) {
    return;
  }
  tracking_ = false;
  SetDocumentDragCapture(false);
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  const float dy_dp = PixelDeltaToDp(drag_last_y_px_ - drag_start_y_px_);
  const float height_dp = ResolveSheetHeightDp();
  const float threshold = height_dp * kDismissThresholdRatio;
  if (dy_dp >= threshold && on_dismiss_) {
    SetSheetOffset(height_dp, true);
    on_dismiss_();
    return;
  }
  SetSheetOffset(0.f, true);
}

void ShellBottomSheetGesture::ProcessEvent(Rml::Event& event) {
  if (!sheet_) {
    return;
  }

  switch (event.GetId()) {
  case Rml::EventId::Mousedown:
    if (ShouldIgnoreTarget(event.GetTargetElement())) {
      return;
    }
    if (!ShouldStartSwipe(event.GetTargetElement())) {
      return;
    }
    BeginDrag(EventMouseY(event));
    break;
  case Rml::EventId::Mousemove:
    UpdateDrag(EventMouseY(event), event);
    break;
  case Rml::EventId::Mouseup:
    EndDrag();
    break;
  default:
    break;
  }
}

} // namespace pbr
