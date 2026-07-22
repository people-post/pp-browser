#include "feature/ui/ShellChatOverlayGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ID.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr float kDismissThresholdRatio = 0.30f;
constexpr float kEdgeSwipeWidthDp = 20.f;
constexpr int kDragDeadzonePx = 8;

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
  document_ = overlay_->GetOwnerDocument();
  context_ = context;
  shell_width_dp_ = shell_width_dp;
  on_dismiss_ = std::move(on_dismiss);
  overlay_->AddEventListener(Rml::EventId::Mousedown, this);
  attached_ = true;
  SetOverlayOffset(0.f, false);
}

void ShellChatOverlayGesture::Detach() {
  SetDocumentDragCapture(false);
  if (attached_ && overlay_) {
    overlay_->RemoveEventListener(Rml::EventId::Mousedown, this);
  }
  overlay_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  attached_ = false;
  tracking_ = false;
  dragging_ = false;
}

void ShellChatOverlayGesture::SetDocumentDragCapture(bool enabled) {
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

float ShellChatOverlayGesture::PixelDeltaToDp(int delta_px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  if (ratio <= 0.f) {
    return static_cast<float>(delta_px);
  }
  return static_cast<float>(delta_px) / ratio;
}

float ShellChatOverlayGesture::ResolveOverlayWidthDp() const {
  if (overlay_ && context_) {
    const float ratio = context_->GetDensityIndependentPixelRatio();
    const float width_px = overlay_->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (ratio > 0.f && width_px > 0.f) {
      return width_px / ratio;
    }
  }
  return shell_width_dp_;
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

bool ShellChatOverlayGesture::ShouldStartSwipe(Rml::Element* target, int x_px) const {
  if (!context_) {
    return false;
  }
  const float edge_px = kEdgeSwipeWidthDp * context_->GetDensityIndependentPixelRatio();
  if (static_cast<float>(x_px) <= edge_px) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node->IsClassSet("shell-chat-overlay-chrome") || node->IsClassSet("shell-back-btn")) {
      return true;
    }
  }
  return false;
}

void ShellChatOverlayGesture::SetOverlayOffset(float dx_dp, bool animate) {
  if (!overlay_) {
    return;
  }
  overlay_->SetClass("shell-chat-overlay--dragging", !animate);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateX(%.1fdp)", dx_dp < 0.f ? 0.f : dx_dp);
  overlay_->SetProperty("transform", buffer);
}

void ShellChatOverlayGesture::BeginDrag(int x_px) {
  tracking_ = true;
  dragging_ = false;
  drag_start_x_px_ = x_px;
  drag_last_x_px_ = x_px;
  SetDocumentDragCapture(true);
}

void ShellChatOverlayGesture::UpdateDrag(int x_px, Rml::Event& event) {
  if (!tracking_ || !overlay_) {
    return;
  }
  drag_last_x_px_ = x_px;
  const int dx_px = x_px - drag_start_x_px_;
  if (!dragging_) {
    if (dx_px > kDragDeadzonePx) {
      dragging_ = true;
      overlay_->SetClass("shell-chat-overlay--dragging", true);
    } else {
      return;
    }
  }
  const float dx_dp = PixelDeltaToDp(dx_px);
  if (dx_dp > 0.f) {
    SetOverlayOffset(dx_dp, false);
    event.StopPropagation();
  } else {
    SetOverlayOffset(0.f, false);
  }
}

void ShellChatOverlayGesture::EndDrag() {
  if (!tracking_) {
    return;
  }
  tracking_ = false;
  SetDocumentDragCapture(false);
  if (!dragging_) {
    return;
  }
  dragging_ = false;
  const float dx_dp = PixelDeltaToDp(drag_last_x_px_ - drag_start_x_px_);
  const float width_dp = ResolveOverlayWidthDp();
  const float threshold = width_dp * kDismissThresholdRatio;
  if (dx_dp >= threshold && on_dismiss_) {
    SetOverlayOffset(width_dp, true);
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
