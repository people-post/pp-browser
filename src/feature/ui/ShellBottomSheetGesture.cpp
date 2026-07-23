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
constexpr float kScrollTopEpsilonPx = 1.f;

int EventMouseX(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

} // namespace

void ShellBottomSheetGesture::Attach(Rml::Element* sheet, Rml::Context* context, float sheet_height_dp,
                                     DismissCallback on_dismiss, ShellGestureAxisLock* axis_lock) {
  Detach();
  if (!sheet || !context) {
    return;
  }
  sheet_ = sheet;
  document_ = sheet_->GetOwnerDocument();
  context_ = context;
  sheet_height_dp_ = sheet_height_dp;
  on_dismiss_ = std::move(on_dismiss);
  axis_lock_ = axis_lock;
  sheet_->AddEventListener(Rml::EventId::Mousedown, this);
  attached_ = true;
  SetSheetOffset(0.f, false);
}

void ShellBottomSheetGesture::Detach() {
  Abort();
  SetClickSuppress(false);
  SetDismissOwnsTopOverscroll(false);
  if (attached_ && sheet_) {
    sheet_->RemoveEventListener(Rml::EventId::Mousedown, this);
  }
  sheet_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  arm_target_ = nullptr;
  on_dismiss_ = {};
  axis_lock_ = nullptr;
  attached_ = false;
  suppress_click_ = false;
}

void ShellBottomSheetGesture::Abort() {
  AbortArm(true);
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

void ShellBottomSheetGesture::SetClickSuppress(bool enabled) {
  if (!document_ || click_suppress_listener_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(Rml::EventId::Click, this, true);
  } else {
    document_->RemoveEventListener(Rml::EventId::Click, this, true);
  }
  click_suppress_listener_ = enabled;
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
    if (tag == "textarea" || tag == "input" || tag == "select") {
      return true;
    }
    if (tag == "button") {
      // Disclosure rows stay eligible for scroll-aware dismiss; action buttons do not.
      if (node->IsClassSet("settings-row")) {
        continue;
      }
      return true;
    }
    if (node->IsClassSet("btn") || node->IsClassSet("shell-close-btn") || node->IsClassSet("settings-back-btn")) {
      return true;
    }
  }
  return false;
}

bool ShellBottomSheetGesture::IsUnderSheet(Rml::Element* target) const {
  if (!sheet_ || !target) {
    return false;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == sheet_) {
      return true;
    }
  }
  return false;
}

bool ShellBottomSheetGesture::IsChromeRegion(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == sheet_) {
      break;
    }
    if (node->IsClassSet("shell-account-sheet-grabber") || node->IsClassSet("shell-account-sheet-header")) {
      return true;
    }
  }
  return false;
}

bool ShellBottomSheetGesture::ScrollAncestorsAtTop(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f) {
      if (node->GetScrollTop() > kScrollTopEpsilonPx) {
        return false;
      }
    }
    if (node == sheet_) {
      break;
    }
  }
  return true;
}

void ShellBottomSheetGesture::PinScrollAncestorsAtTop() {
  if (!arm_target_) {
    return;
  }
  for (Rml::Element* node = arm_target_; node; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f && node->GetScrollTop() < kScrollTopEpsilonPx) {
      node->SetScrollTop(0.f);
    }
    if (node == sheet_) {
      break;
    }
  }
  if (context_) {
    context_->ClearScrollOverscroll();
  }
}

void ShellBottomSheetGesture::SetDismissOwnsTopOverscroll(bool owns) {
  if (dismiss_owns_top_overscroll_ == owns) {
    return;
  }
  dismiss_owns_top_overscroll_ = owns;
  if (!context_) {
    return;
  }
  if (owns) {
    // Block top (min-y) rubber-band so pull-down belongs to sheet dismiss.
    context_->SetScrollOverscrollEdges(true, true, false, true);
    PinScrollAncestorsAtTop();
  } else {
    context_->SetScrollOverscrollEdges(true, true, true, true);
  }
}

bool ShellBottomSheetGesture::ShouldStartSwipe(Rml::Element* target) const {
  if (!IsUnderSheet(target)) {
    return false;
  }
  if (IsChromeRegion(target)) {
    return true;
  }
  return ScrollAncestorsAtTop(target);
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

void ShellBottomSheetGesture::BeginArm(int x_px, int y_px, Rml::Element* target) {
  tracking_ = true;
  dragging_ = false;
  arm_target_ = target;
  drag_start_x_px_ = x_px;
  drag_start_y_px_ = y_px;
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  SetDismissOwnsTopOverscroll(true);
  SetDocumentDragCapture(true);
}

void ShellBottomSheetGesture::AbortArm(bool unlock_axis) {
  if (!tracking_ && !dragging_) {
    SetDocumentDragCapture(false);
    SetDismissOwnsTopOverscroll(false);
    arm_target_ = nullptr;
    if (unlock_axis && axis_lock_ && axis_lock_->Get() == ShellGestureAxis::Vertical) {
      axis_lock_->Unlock();
    }
    return;
  }
  tracking_ = false;
  dragging_ = false;
  SetDocumentDragCapture(false);
  SetDismissOwnsTopOverscroll(false);
  arm_target_ = nullptr;
  SetSheetOffset(0.f, true);
  if (unlock_axis && axis_lock_ && axis_lock_->Get() == ShellGestureAxis::Vertical) {
    axis_lock_->Unlock();
  }
}

void ShellBottomSheetGesture::UpdateDrag(int x_px, int y_px, Rml::Event& event) {
  if (!tracking_ || !sheet_) {
    return;
  }
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  const int dx_px = x_px - drag_start_x_px_;
  const int dy_px = y_px - drag_start_y_px_;

  if (axis_lock_) {
    const ShellGestureAxis axis = axis_lock_->Observe(dx_px, dy_px, kDragDeadzonePx);
    if (axis == ShellGestureAxis::None) {
      return;
    }
    if (axis == ShellGestureAxis::Horizontal) {
      // Swipe-back won — release without unlocking.
      tracking_ = false;
      dragging_ = false;
      SetDocumentDragCapture(false);
      SetSheetOffset(0.f, false);
      return;
    }
  } else {
    if (!dragging_) {
      if (dy_px < -kDragDeadzonePx) {
        AbortArm(true);
        return;
      }
      if (dy_px <= kDragDeadzonePx) {
        return;
      }
    }
  }

  if (!dragging_) {
    if (dy_px < -kDragDeadzonePx) {
      // Clear upward intent: release so overflow scroll can own the gesture.
      AbortArm(axis_lock_ == nullptr || axis_lock_->Get() != ShellGestureAxis::Vertical);
      return;
    }
    if (dy_px <= kDragDeadzonePx && (!axis_lock_ || axis_lock_->Get() == ShellGestureAxis::None)) {
      return;
    }
    if (dy_px <= 0) {
      return;
    }
    dragging_ = true;
    suppress_click_ = true;
    SetClickSuppress(true);
    sheet_->SetClass("shell-account-sheet--dragging", true);
  }

  // While dismiss owns the pull-down, keep list scroll pinned at top (no competing rubber-band).
  if (dy_px > 0) {
    PinScrollAncestorsAtTop();
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
    if (axis_lock_ && axis_lock_->Get() == ShellGestureAxis::Vertical) {
      axis_lock_->Unlock();
    }
    SetDismissOwnsTopOverscroll(false);
    arm_target_ = nullptr;
    return;
  }
  tracking_ = false;
  SetDocumentDragCapture(false);
  const bool was_dragging = dragging_;
  dragging_ = false;
  SetDismissOwnsTopOverscroll(false);
  arm_target_ = nullptr;
  if (axis_lock_ && axis_lock_->Get() == ShellGestureAxis::Vertical) {
    axis_lock_->Unlock();
  }
  if (!was_dragging) {
    return;
  }
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
    BeginArm(EventMouseX(event), EventMouseY(event), event.GetTargetElement());
    break;
  case Rml::EventId::Mousemove:
    UpdateDrag(EventMouseX(event), EventMouseY(event), event);
    break;
  case Rml::EventId::Mouseup:
    EndDrag();
    break;
  case Rml::EventId::Click:
    if (suppress_click_) {
      event.StopImmediatePropagation();
      suppress_click_ = false;
      SetClickSuppress(false);
    }
    break;
  default:
    break;
  }
}

} // namespace pbr
