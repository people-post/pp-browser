#include "feature/ui/ShellChatOverlayGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ID.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr float kDismissThresholdRatio = 0.30f;
constexpr float kEdgeSwipeWidthDp = 20.f;

int EventMouseX(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

} // namespace

void ShellChatOverlayGesture::Attach(Rml::Element* overlay, Rml::Context* context, float shell_width_dp,
                                     DismissCallback on_dismiss) {
  Detach();
  if (!overlay || !context) {
    return;
  }
  overlay_ = overlay;
  context_ = context;
  shell_width_dp_ = shell_width_dp;
  on_dismiss_ = std::move(on_dismiss);
  overlay_->AddEventListener(Rml::EventId::Mousedown, this);
  overlay_->AddEventListener(Rml::EventId::Mousemove, this);
  overlay_->AddEventListener(Rml::EventId::Mouseup, this);
  attached_ = true;
  SetOverlayOffset(0.f, false);
}

void ShellChatOverlayGesture::Detach() {
  if (attached_ && overlay_) {
    overlay_->RemoveEventListener(Rml::EventId::Mousedown, this);
    overlay_->RemoveEventListener(Rml::EventId::Mousemove, this);
    overlay_->RemoveEventListener(Rml::EventId::Mouseup, this);
  }
  overlay_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  attached_ = false;
  tracking_ = false;
  dragging_ = false;
}

bool ShellChatOverlayGesture::ShouldIgnoreTarget(Rml::Element* target) const {
  if (!target) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select" || tag == "button") {
      return true;
    }
    const Rml::String& id = node->GetId();
    if (id == "draft-input") {
      return true;
    }
  }
  return false;
}

bool ShellChatOverlayGesture::ShouldStartSwipe(Rml::Element* target, int x) const {
  if (!context_) {
    return false;
  }
  const float edge_px = kEdgeSwipeWidthDp * context_->GetDensityIndependentPixelRatio();
  if (x <= edge_px) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node->IsClassSet("shell-chat-overlay-chrome") || node->IsClassSet("shell-back-btn")) {
      return true;
    }
  }
  return false;
}

void ShellChatOverlayGesture::SetOverlayOffset(float dx, bool animate) {
  if (!overlay_) {
    return;
  }
  overlay_->SetClass("shell-chat-overlay--dragging", !animate);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateX(%.1fdp)", dx < 0.f ? 0.f : dx);
  overlay_->SetProperty("transform", buffer);
}

void ShellChatOverlayGesture::BeginDrag(int x) {
  tracking_ = true;
  dragging_ = false;
  drag_start_x_ = x;
  drag_last_x_ = x;
}

void ShellChatOverlayGesture::UpdateDrag(int x, Rml::Event& event) {
  if (!tracking_ || !overlay_) {
    return;
  }
  drag_last_x_ = x;
  const int dx = x - drag_start_x_;
  if (!dragging_) {
    if (dx > 8) {
      dragging_ = true;
      overlay_->SetClass("shell-chat-overlay--dragging", true);
    } else {
      return;
    }
  }
  if (dx > 0) {
    SetOverlayOffset(static_cast<float>(dx), false);
    event.StopPropagation();
  }
}

void ShellChatOverlayGesture::EndDrag() {
  if (!tracking_) {
    return;
  }
  tracking_ = false;
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  const float dx = static_cast<float>(drag_last_x_ - drag_start_x_);
  const float threshold = shell_width_dp_ * kDismissThresholdRatio;
  if (dx >= threshold && on_dismiss_) {
    SetOverlayOffset(shell_width_dp_, true);
    on_dismiss_();
    return;
  }
  SetOverlayOffset(0.f, true);
}

void ShellChatOverlayGesture::ProcessEvent(Rml::Event& event) {
  if (!overlay_) {
    return;
  }

  switch (event.GetId()) {
  case Rml::EventId::Mousedown:
    if (ShouldIgnoreTarget(event.GetTargetElement())) {
      return;
    }
    if (!ShouldStartSwipe(event.GetTargetElement(), EventMouseX(event))) {
      return;
    }
    BeginDrag(EventMouseX(event));
    break;
  case Rml::EventId::Mousemove:
    UpdateDrag(EventMouseX(event), event);
    break;
  case Rml::EventId::Mouseup:
    EndDrag();
    break;
  default:
    break;
  }
}

} // namespace pbr
