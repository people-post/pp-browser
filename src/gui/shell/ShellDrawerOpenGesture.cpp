#include "gui/shell/ShellDrawerOpenGesture.h"

#include <ui/base/ID.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

namespace pbr {

namespace {

constexpr int kDeadzonePx = 8;

int EventMouseX(const ui::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const ui::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

} // namespace

void ShellDrawerOpenGesture::Attach(ui::Element* surface, ui::Context* context, AttachOptions options,
                                    std::function<bool()> can_open, std::function<void()> on_open) {
  Detach();
  if (!surface || !context) {
    return;
  }
  surface_ = surface;
  document_ = surface->GetOwnerDocument();
  context_ = context;
  options_ = std::move(options);
  can_open_ = std::move(can_open);
  on_open_ = std::move(on_open);
  surface_->AddEventListener(ui::EventId::Mousedown, this);
  surface_->AddEventListener(ui::EventId::Click, this, true);
}

void ShellDrawerOpenGesture::Detach() {
  Stop();
  if (surface_) {
    surface_->RemoveEventListener(ui::EventId::Mousedown, this);
    surface_->RemoveEventListener(ui::EventId::Click, this, true);
  }
  surface_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  can_open_ = {};
  on_open_ = {};
  swallow_click_ = false;
}

void ShellDrawerOpenGesture::Stop() {
  tracking_ = false;
  axis_lock_.Unlock();
  SetDocumentCapture(false);
}

void ShellDrawerOpenGesture::SetDocumentCapture(const bool enabled) {
  if (!document_ || capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  capture_ = enabled;
}

float ShellDrawerOpenGesture::PxToDp(const int px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? static_cast<float>(px) / ratio : static_cast<float>(px);
}

bool ShellDrawerOpenGesture::Excluded(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    const ui::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select") {
      return true;
    }
    for (const std::string& id : options_.excluded_ids) {
      if (node->GetId() == id.c_str()) {
        return true;
      }
    }
    for (const std::string& cls : options_.excluded_classes) {
      if (node->IsClassSet(cls.c_str())) {
        return true;
      }
    }
    if (node == surface_) {
      break;
    }
  }
  return false;
}

void ShellDrawerOpenGesture::ProcessEvent(ui::Event& event) {
  if (!surface_ || !context_) {
    return;
  }
  switch (event.GetId()) {
  case ui::EventId::Click:
    if (swallow_click_) {
      const bool same_sequence =
          std::chrono::steady_clock::now() - swallow_click_since_ < std::chrono::milliseconds(300);
      swallow_click_ = false;
      if (same_sequence) {
        event.StopPropagation();
      }
    }
    break;
  case ui::EventId::Mousedown:
    swallow_click_ = false;
    Stop();
    if (Excluded(event.GetTargetElement()) || !can_open_ || !can_open_()) {
      return;
    }
    tracking_ = true;
    start_x_px_ = EventMouseX(event);
    start_y_px_ = EventMouseY(event);
    SetDocumentCapture(true);
    break;
  case ui::EventId::Mousemove: {
    if (!tracking_) {
      return;
    }
    const int dx_px = EventMouseX(event) - start_x_px_;
    const int dy_px = EventMouseY(event) - start_y_px_;
    const ShellGestureAxis axis = axis_lock_.Observe(dx_px, dy_px, kDeadzonePx);
    if (axis == ShellGestureAxis::None) {
      return;
    }
    if (axis == ShellGestureAxis::Vertical || dx_px < 0) {
      Stop();  // a scroll, or a drag to the left: not ours
      return;
    }
    if (!Opens(PxToDp(dx_px), PxToDp(dy_px))) {
      return;
    }
    Stop();
    swallow_click_ = true;
    swallow_click_since_ = std::chrono::steady_clock::now();
    if (can_open_ && can_open_() && on_open_) {
      on_open_();
    }
    break;
  }
  case ui::EventId::Mouseup:
    Stop();
    break;
  default:
    break;
  }
}

} // namespace pbr
