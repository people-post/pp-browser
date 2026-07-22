#include "feature/ui/ShellBottomSheetGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr float kDismissThresholdRatio = 0.25f;

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
  context_ = context;
  sheet_height_dp_ = sheet_height_dp;
  on_dismiss_ = std::move(on_dismiss);
  sheet_->AddEventListener(Rml::EventId::Mousedown, this);
  sheet_->AddEventListener(Rml::EventId::Mousemove, this);
  sheet_->AddEventListener(Rml::EventId::Mouseup, this);
  attached_ = true;
  SetSheetOffset(0.f, false);
}

void ShellBottomSheetGesture::Detach() {
  if (attached_ && sheet_) {
    sheet_->RemoveEventListener(Rml::EventId::Mousedown, this);
    sheet_->RemoveEventListener(Rml::EventId::Mousemove, this);
    sheet_->RemoveEventListener(Rml::EventId::Mouseup, this);
  }
  sheet_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  attached_ = false;
  tracking_ = false;
  dragging_ = false;
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

void ShellBottomSheetGesture::SetSheetOffset(float dy, bool animate) {
  if (!sheet_) {
    return;
  }
  sheet_->SetClass("shell-account-sheet--dragging", !animate);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateY(%.1fdp)", dy < 0.f ? 0.f : dy);
  sheet_->SetProperty("transform", buffer);
}

void ShellBottomSheetGesture::BeginDrag(int y) {
  tracking_ = true;
  dragging_ = false;
  drag_start_y_ = y;
  drag_last_y_ = y;
}

void ShellBottomSheetGesture::UpdateDrag(int y, Rml::Event& event) {
  if (!tracking_ || !sheet_) {
    return;
  }
  drag_last_y_ = y;
  const int dy = y - drag_start_y_;
  if (!dragging_) {
    if (dy > 8) {
      dragging_ = true;
      sheet_->SetClass("shell-account-sheet--dragging", true);
    } else {
      return;
    }
  }
  if (dy > 0) {
    SetSheetOffset(static_cast<float>(dy), false);
    event.StopPropagation();
  }
}

void ShellBottomSheetGesture::EndDrag() {
  if (!tracking_) {
    return;
  }
  tracking_ = false;
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  const float dy = static_cast<float>(drag_last_y_ - drag_start_y_);
  const float threshold = sheet_height_dp_ * kDismissThresholdRatio;
  if (dy >= threshold && on_dismiss_) {
    SetSheetOffset(sheet_height_dp_, true);
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
