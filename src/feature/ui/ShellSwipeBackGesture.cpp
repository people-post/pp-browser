#include "feature/ui/ShellSwipeBackGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ID.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr float kDismissThresholdRatio = 0.30f;
constexpr float kEdgeSwipeWidthDp = 28.f;
constexpr int kDragDeadzonePx = 8;

int EventMouseX(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

} // namespace

void ShellSwipeBackGesture::Attach(Rml::Element* listen_surface, Rml::Context* context, AttachOptions options,
                                   DismissCallback on_dismiss) {
  Detach();
  if (!listen_surface || !context) {
    return;
  }
  surface_ = listen_surface;
  transform_target_ = options.transform_target ? options.transform_target : listen_surface;
  content_root_ = options.content_root;
  document_ = surface_->GetOwnerDocument();
  context_ = context;
  options_ = std::move(options);
  on_dismiss_ = std::move(on_dismiss);
  surface_->AddEventListener(Rml::EventId::Mousedown, this);
  attached_ = true;
  SetSurfaceOffset(0.f, false);
}

void ShellSwipeBackGesture::Detach() {
  Abort();
  if (attached_ && surface_) {
    surface_->RemoveEventListener(Rml::EventId::Mousedown, this);
  }
  surface_ = nullptr;
  transform_target_ = nullptr;
  content_root_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  options_ = {};
  attached_ = false;
  from_edge_ = false;
}

void ShellSwipeBackGesture::Abort() {
  if (!tracking_ && !dragging_) {
    SetDocumentDragCapture(false);
    return;
  }
  tracking_ = false;
  dragging_ = false;
  from_edge_ = false;
  SetDocumentDragCapture(false);
  SetSurfaceOffset(0.f, true);
}

Rml::Element* ShellSwipeBackGesture::TransformTarget() const {
  return transform_target_ ? transform_target_ : surface_;
}

bool ShellSwipeBackGesture::IsUnder(Rml::Element* ancestor, Rml::Element* target) const {
  if (!ancestor || !target) {
    return false;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == ancestor) {
      return true;
    }
  }
  return false;
}

void ShellSwipeBackGesture::SetDocumentDragCapture(bool enabled) {
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

float ShellSwipeBackGesture::PixelDeltaToDp(int delta_px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  if (ratio <= 0.f) {
    return static_cast<float>(delta_px);
  }
  return static_cast<float>(delta_px) / ratio;
}

float ShellSwipeBackGesture::ResolveSurfaceWidthDp() const {
  Rml::Element* target = TransformTarget();
  if (target && context_) {
    const float ratio = context_->GetDensityIndependentPixelRatio();
    const float width_px = target->GetBox().GetSize(Rml::BoxArea::Border).x;
    if (ratio > 0.f && width_px > 0.f) {
      return width_px / ratio;
    }
  }
  return options_.width_dp_fallback;
}

bool ShellSwipeBackGesture::ShouldIgnoreTarget(Rml::Element* target) const {
  if (!target) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select") {
      return true;
    }
    if (tag == "button") {
      // Back chrome may start a swipe; other buttons should not.
      if (IsChromeRegion(node)) {
        continue;
      }
      return true;
    }
    const Rml::String& id = node->GetId();
    if (id == "draft-input") {
      return true;
    }
  }
  return false;
}

bool ShellSwipeBackGesture::IsChromeRegion(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == surface_) {
      break;
    }
    for (const std::string& cls : options_.chrome_classes) {
      if (node->IsClassSet(cls.c_str())) {
        return true;
      }
    }
  }
  return false;
}

bool ShellSwipeBackGesture::ShouldStartSwipe(Rml::Element* target, int x_px) const {
  if (!context_ || !surface_) {
    return false;
  }
  if (IsChromeRegion(target)) {
    return true;
  }
  // Optional content root: ignore presses that aren't on the drill-down (or its listen parent).
  if (content_root_ && !IsUnder(content_root_, target) && !IsUnder(surface_, target)) {
    return false;
  }
  if (!options_.require_edge) {
    return true;
  }
  const float edge_px = kEdgeSwipeWidthDp * context_->GetDensityIndependentPixelRatio();
  // Listen-surface left so parent padding still counts as the edge strip.
  const float surface_left = surface_->GetAbsoluteLeft();
  return static_cast<float>(x_px) <= surface_left + edge_px;
}

void ShellSwipeBackGesture::SetSurfaceOffset(float dx_dp, bool animate) {
  Rml::Element* target = TransformTarget();
  if (!target) {
    return;
  }
  if (options_.dragging_class && options_.dragging_class[0] != '\0') {
    target->SetClass(options_.dragging_class, !animate);
  }
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateX(%.1fdp)", dx_dp < 0.f ? 0.f : dx_dp);
  target->SetProperty("transform", buffer);
}

void ShellSwipeBackGesture::BeginDrag(int x_px, int y_px, bool from_edge) {
  tracking_ = true;
  dragging_ = false;
  from_edge_ = from_edge;
  drag_start_x_px_ = x_px;
  drag_start_y_px_ = y_px;
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  if (from_edge && options_.axis_lock) {
    options_.axis_lock->SetPreferHorizontal(true);
  }
  SetDocumentDragCapture(true);
}

void ShellSwipeBackGesture::UpdateDrag(int x_px, int y_px, Rml::Event& event) {
  if (!tracking_ || !TransformTarget()) {
    return;
  }
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  const int dx_px = x_px - drag_start_x_px_;
  const int dy_px = y_px - drag_start_y_px_;

  if (options_.axis_lock) {
    const ShellGestureAxis axis = options_.axis_lock->Observe(dx_px, dy_px, kDragDeadzonePx);
    if (axis == ShellGestureAxis::None) {
      return;
    }
    if (axis == ShellGestureAxis::Vertical) {
      // Sheet dismiss won — release without unlocking (winner holds the lock).
      tracking_ = false;
      dragging_ = false;
      from_edge_ = false;
      SetDocumentDragCapture(false);
      SetSurfaceOffset(0.f, false);
      return;
    }
  } else if (!dragging_) {
    if (dx_px <= kDragDeadzonePx) {
      return;
    }
  }

  if (!dragging_) {
    dragging_ = true;
    if (options_.dragging_class && options_.dragging_class[0] != '\0') {
      TransformTarget()->SetClass(options_.dragging_class, true);
    }
  }

  const float dx_dp = PixelDeltaToDp(dx_px);
  if (dx_dp > 0.f) {
    SetSurfaceOffset(dx_dp, false);
    event.StopPropagation();
  } else {
    SetSurfaceOffset(0.f, false);
  }
}

void ShellSwipeBackGesture::EndDrag() {
  if (!tracking_) {
    if (options_.axis_lock && options_.axis_lock->Get() == ShellGestureAxis::Horizontal) {
      options_.axis_lock->Unlock();
    }
    from_edge_ = false;
    return;
  }
  tracking_ = false;
  SetDocumentDragCapture(false);
  const bool was_dragging = dragging_;
  dragging_ = false;
  from_edge_ = false;
  if (options_.axis_lock && options_.axis_lock->Get() == ShellGestureAxis::Horizontal) {
    options_.axis_lock->Unlock();
  }
  if (!was_dragging) {
    return;
  }
  const float dx_dp = PixelDeltaToDp(drag_last_x_px_ - drag_start_x_px_);
  const float width_dp = ResolveSurfaceWidthDp();
  const float threshold = width_dp * kDismissThresholdRatio;
  if (dx_dp >= threshold && on_dismiss_) {
    SetSurfaceOffset(width_dp, true);
    on_dismiss_();
    return;
  }
  SetSurfaceOffset(0.f, true);
}

void ShellSwipeBackGesture::ProcessEvent(Rml::Event& event) {
  if (!surface_) {
    return;
  }

  switch (event.GetId()) {
  case Rml::EventId::Mousedown: {
    Rml::Element* target = event.GetTargetElement();
    if (ShouldIgnoreTarget(target)) {
      return;
    }
    if (!ShouldStartSwipe(target, EventMouseX(event))) {
      return;
    }
    const float edge_px = kEdgeSwipeWidthDp * context_->GetDensityIndependentPixelRatio();
    const float surface_left = surface_->GetAbsoluteLeft();
    const bool from_edge = static_cast<float>(EventMouseX(event)) <= surface_left + edge_px;
    BeginDrag(EventMouseX(event), EventMouseY(event), from_edge);
    break;
  }
  case Rml::EventId::Mousemove:
    UpdateDrag(EventMouseX(event), EventMouseY(event), event);
    break;
  case Rml::EventId::Mouseup:
    EndDrag();
    break;
  default:
    break;
  }
}

} // namespace pbr
