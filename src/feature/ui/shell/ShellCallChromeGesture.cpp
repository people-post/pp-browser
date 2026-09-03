#include "feature/ui/shell/ShellCallChromeGesture.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

constexpr float kModeThresholdDp = 56.f;
constexpr int kDragDeadzonePx = 8;
constexpr float kScrollTopEpsilonPx = 1.f;
constexpr float kTapSlopDp = 10.f;

int EventMouseX(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const Rml::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

const char* CornerClass(int corner) {
  switch (corner) {
  case 1:
    return "shell-call-minimized-chip--tl";
  case 2:
    return "shell-call-minimized-chip--br";
  case 3:
    return "shell-call-minimized-chip--bl";
  case 0:
  default:
    return "shell-call-minimized-chip--tr";
  }
}

} // namespace

void ShellCallChromeGesture::Attach(Rml::Element* root, Rml::Context* context, CallChromeMode mode,
                                    Callbacks callbacks, ShellGestureAxisLock* axis_lock) {
  Detach();
  if (!root || !context) {
    return;
  }
  root_ = root;
  document_ = root_->GetOwnerDocument();
  context_ = context;
  mode_ = mode;
  callbacks_ = std::move(callbacks);
  axis_lock_ = axis_lock;
  root_->AddEventListener(Rml::EventId::Mousedown, this);
  attached_ = true;
  if (mode_ == CallChromeMode::Minimized) {
    SetChipOffset(0.f, 0.f, false);
  } else if (mode_ == CallChromeMode::Expanded) {
    SetOutsideTapCapture(true);
  } else {
    SetRootOffsetY(0.f, false);
  }
}

void ShellCallChromeGesture::Detach() {
  Abort();
  SetClickSuppress(false);
  SetOutsideTapCapture(false);
  SetDismissOwnsTopOverscroll(false);
  if (attached_ && root_) {
    root_->RemoveEventListener(Rml::EventId::Mousedown, this);
  }
  root_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  arm_target_ = nullptr;
  callbacks_ = {};
  axis_lock_ = nullptr;
  attached_ = false;
  suppress_click_ = false;
}

void ShellCallChromeGesture::Abort() {
  AbortArm(true);
}

void ShellCallChromeGesture::SetDocumentDragCapture(bool enabled) {
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

void ShellCallChromeGesture::UpdateDocumentClickCapture() {
  if (!document_) {
    return;
  }
  const bool enabled = outside_tap_capture_ || click_suppress_listener_;
  if (document_click_capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(Rml::EventId::Click, this, true);
  } else {
    document_->RemoveEventListener(Rml::EventId::Click, this, true);
  }
  document_click_capture_ = enabled;
}

void ShellCallChromeGesture::SetOutsideTapCapture(bool enabled) {
  if (outside_tap_capture_ == enabled) {
    return;
  }
  outside_tap_capture_ = enabled;
  UpdateDocumentClickCapture();
}

void ShellCallChromeGesture::SetClickSuppress(bool enabled) {
  if (click_suppress_listener_ == enabled) {
    return;
  }
  click_suppress_listener_ = enabled;
  UpdateDocumentClickCapture();
}

float ShellCallChromeGesture::PixelDeltaToDp(int delta_px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  if (ratio <= 0.f) {
    return static_cast<float>(delta_px);
  }
  return static_cast<float>(delta_px) / ratio;
}

bool ShellCallChromeGesture::ShouldIgnoreTarget(Rml::Element* target) const {
  if (!target) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select") {
      return true;
    }
    if (tag == "button") {
      return true;
    }
    if (node->IsClassSet("btn") || node->IsClassSet("shell-close-btn")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::ShouldIgnoreOutsideDismiss(Rml::Element* target) const {
  if (!target) {
    return true;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::String& id = node->GetId();
    if (id == "shell-call-ring-mount" || id == "shell-dialog-mount" || id == "shell-pin-gate-mount") {
      return true;
    }
    if (node->IsClassSet("shell-overlay-chrome-mount") || node->IsClassSet("shell-account-sheet-scrim") ||
        node->IsClassSet("shell-sheet-scrim") || node->IsClassSet("shell-scrim")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::IsUnderRoot(Rml::Element* target) const {
  if (!root_ || !target) {
    return false;
  }
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == root_) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::IsScrollRegion(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node == root_) {
      break;
    }
    if (node->IsClassSet("shell-call-immersive-roster")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::ScrollAncestorsAtTop(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f) {
      if (node->GetScrollTop() > kScrollTopEpsilonPx) {
        return false;
      }
    }
    if (node == root_) {
      break;
    }
  }
  return true;
}

bool ShellCallChromeGesture::ShouldArmImmersivePullDown(Rml::Element* target) const {
  if (!IsUnderRoot(target) || ShouldIgnoreTarget(target)) {
    return false;
  }
  if (IsScrollRegion(target)) {
    return ScrollAncestorsAtTop(target);
  }
  return true;
}

void ShellCallChromeGesture::PinScrollAncestorsAtTop() {
  if (!arm_target_) {
    return;
  }
  for (Rml::Element* node = arm_target_; node; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f && node->GetScrollTop() < kScrollTopEpsilonPx) {
      node->SetScrollTop(0.f);
    }
    if (node == root_) {
      break;
    }
  }
  if (context_) {
    context_->ClearScrollOverscroll();
  }
}

void ShellCallChromeGesture::SetDismissOwnsTopOverscroll(bool owns) {
  if (dismiss_owns_top_overscroll_ == owns) {
    return;
  }
  dismiss_owns_top_overscroll_ = owns;
  if (!context_) {
    return;
  }
  if (owns) {
    context_->SetScrollOverscrollEdges(true, true, false, true);
    PinScrollAncestorsAtTop();
  } else {
    context_->SetScrollOverscrollEdges(true, true, true, true);
  }
}

void ShellCallChromeGesture::SetRootOffsetY(float dy_dp, bool animate) {
  if (!root_) {
    return;
  }
  root_->SetClass("shell-call-chrome--dragging", !animate);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "translateY(%.1fdp)", dy_dp);
  root_->SetProperty("transform", buffer);
}

void ShellCallChromeGesture::SetChipOffset(float dx_dp, float dy_dp, bool animate) {
  if (!root_) {
    return;
  }
  root_->SetClass("shell-call-chrome--dragging", !animate);
  char buffer[80];
  std::snprintf(buffer, sizeof(buffer), "translate(%.1fdp, %.1fdp)", dx_dp, dy_dp);
  root_->SetProperty("transform", buffer);
}

void ShellCallChromeGesture::ApplyCornerClass(int corner) {
  if (!root_) {
    return;
  }
  root_->SetClass("shell-call-minimized-chip--tr", corner == 0);
  root_->SetClass("shell-call-minimized-chip--tl", corner == 1);
  root_->SetClass("shell-call-minimized-chip--br", corner == 2);
  root_->SetClass("shell-call-minimized-chip--bl", corner == 3);
  (void)CornerClass(corner);
}

int ShellCallChromeGesture::SnapCornerFromChipCenter() const {
  if (!root_ || !context_) {
    return 0;
  }
  const auto box = root_->GetBox().GetSize(Rml::BoxArea::Border);
  const Rml::Vector2f abs = root_->GetAbsoluteOffset(Rml::BoxArea::Border);
  const float cx = abs.x + box.x * 0.5f;
  const float cy = abs.y + box.y * 0.5f;
  const float vw = static_cast<float>(context_->GetDimensions().x);
  const float vh = static_cast<float>(context_->GetDimensions().y);
  const bool left = cx < vw * 0.5f;
  const bool top = cy < vh * 0.5f;
  if (top && !left) {
    return 0;
  }
  if (top && left) {
    return 1;
  }
  if (!top && !left) {
    return 2;
  }
  return 3;
}

void ShellCallChromeGesture::BeginArm(int x_px, int y_px, Rml::Element* target) {
  tracking_ = true;
  dragging_ = false;
  arm_target_ = target;
  drag_start_x_px_ = x_px;
  drag_start_y_px_ = y_px;
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  if (mode_ == CallChromeMode::Immersive) {
    SetDismissOwnsTopOverscroll(true);
  }
  SetDocumentDragCapture(true);
}

void ShellCallChromeGesture::AbortArm(bool unlock_axis) {
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
  if (mode_ == CallChromeMode::Minimized) {
    SetChipOffset(0.f, 0.f, true);
  } else if (mode_ == CallChromeMode::Immersive) {
    SetRootOffsetY(0.f, true);
  }
  if (unlock_axis && axis_lock_ &&
      (axis_lock_->Get() == ShellGestureAxis::Vertical || axis_lock_->Get() == ShellGestureAxis::Horizontal)) {
    axis_lock_->Unlock();
  }
}

void ShellCallChromeGesture::UpdateDrag(int x_px, int y_px, Rml::Event& /*event*/) {
  if (!tracking_ || !root_) {
    return;
  }
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  const int dx_px = x_px - drag_start_x_px_;
  const int dy_px = y_px - drag_start_y_px_;

  if (mode_ == CallChromeMode::Minimized) {
    const float dist_dp = std::sqrt(PixelDeltaToDp(dx_px) * PixelDeltaToDp(dx_px) +
                                    PixelDeltaToDp(dy_px) * PixelDeltaToDp(dy_px));
    if (!dragging_) {
      if (dist_dp <= kTapSlopDp) {
        return;
      }
      dragging_ = true;
      suppress_click_ = true;
      SetClickSuppress(true);
      root_->SetClass("shell-call-chrome--dragging", true);
    }
    SetChipOffset(PixelDeltaToDp(dx_px), PixelDeltaToDp(dy_px), false);
    return;
  }

  if (mode_ == CallChromeMode::Expanded) {
    const float dist_dp = std::sqrt(PixelDeltaToDp(dx_px) * PixelDeltaToDp(dx_px) +
                                    PixelDeltaToDp(dy_px) * PixelDeltaToDp(dy_px));
    if (!dragging_ && dist_dp > kTapSlopDp) {
      dragging_ = true;
      suppress_click_ = true;
      SetClickSuppress(true);
    }
    return;
  }

  if (axis_lock_) {
    const ShellGestureAxis axis = axis_lock_->Observe(dx_px, dy_px, kDragDeadzonePx);
    if (axis == ShellGestureAxis::None) {
      return;
    }
    if (axis == ShellGestureAxis::Horizontal) {
      tracking_ = false;
      dragging_ = false;
      SetDocumentDragCapture(false);
      SetDismissOwnsTopOverscroll(false);
      SetRootOffsetY(0.f, false);
      return;
    }
  }

  if (mode_ == CallChromeMode::Immersive) {
    if (!dragging_) {
      if (dy_px < -kDragDeadzonePx) {
        AbortArm(axis_lock_ == nullptr || axis_lock_->Get() != ShellGestureAxis::Vertical);
        return;
      }
      if (dy_px <= kDragDeadzonePx) {
        return;
      }
      dragging_ = true;
      suppress_click_ = true;
      SetClickSuppress(true);
      root_->SetClass("shell-call-chrome--dragging", true);
    }
    const float dy_dp = std::max(0.f, PixelDeltaToDp(dy_px));
    SetRootOffsetY(dy_dp, false);
    PinScrollAncestorsAtTop();
  }
}

void ShellCallChromeGesture::EndDrag() {
  if (!tracking_) {
    return;
  }
  const bool was_dragging = dragging_;
  const int dy_px = drag_last_y_px_ - drag_start_y_px_;
  const float dy_dp = PixelDeltaToDp(dy_px);

  tracking_ = false;
  dragging_ = false;
  SetDocumentDragCapture(false);
  SetDismissOwnsTopOverscroll(false);
  arm_target_ = nullptr;

  auto unlock = [this]() {
    if (axis_lock_ && axis_lock_->Get() != ShellGestureAxis::None) {
      axis_lock_->Unlock();
    }
  };

  if (mode_ == CallChromeMode::Minimized) {
    if (!was_dragging) {
      SetChipOffset(0.f, 0.f, false);
      unlock();
      if (callbacks_.on_restore) {
        callbacks_.on_restore();
      }
      return;
    }
    const int corner = SnapCornerFromChipCenter();
    ApplyCornerClass(corner);
    SetChipOffset(0.f, 0.f, true);
    unlock();
    if (callbacks_.on_chip_corner) {
      callbacks_.on_chip_corner(corner);
    }
    return;
  }

  if (mode_ == CallChromeMode::Immersive) {
    SetRootOffsetY(0.f, true);
    unlock();
    if (was_dragging && dy_dp >= kModeThresholdDp && callbacks_.on_expand) {
      callbacks_.on_expand();
    }
    return;
  }

  // Expanded: tap chrome → Immersive.
  unlock();
  if (!was_dragging && callbacks_.on_immersive) {
    callbacks_.on_immersive();
  }
}

void ShellCallChromeGesture::ProcessEvent(Rml::Event& event) {
  switch (event.GetId()) {
  case Rml::EventId::Mousedown: {
    if (tracking_) {
      return;
    }
    Rml::Element* target = event.GetTargetElement();
    if (mode_ == CallChromeMode::Minimized) {
      if (!IsUnderRoot(target) || ShouldIgnoreTarget(target)) {
        return;
      }
      BeginArm(EventMouseX(event), EventMouseY(event), target);
      return;
    }
    if (mode_ == CallChromeMode::Immersive) {
      if (!ShouldArmImmersivePullDown(target)) {
        return;
      }
      BeginArm(EventMouseX(event), EventMouseY(event), target);
      return;
    }
    // Expanded — only arm on call chrome under root, not buttons.
    if (!IsUnderRoot(target) || ShouldIgnoreTarget(target)) {
      return;
    }
    BeginArm(EventMouseX(event), EventMouseY(event), target);
    break;
  }
  case Rml::EventId::Mousemove:
    if (tracking_) {
      UpdateDrag(EventMouseX(event), EventMouseY(event), event);
    }
    break;
  case Rml::EventId::Mouseup:
    if (tracking_) {
      EndDrag();
    }
    break;
  case Rml::EventId::Click:
    if (mode_ == CallChromeMode::Expanded && outside_tap_capture_) {
      Rml::Element* target = event.GetTargetElement();
      if (!IsUnderRoot(target) && !ShouldIgnoreOutsideDismiss(target)) {
        if (callbacks_.on_minimize) {
          callbacks_.on_minimize();
        }
        event.StopPropagation();
        event.StopImmediatePropagation();
        return;
      }
    }
    if (suppress_click_) {
      event.StopPropagation();
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
