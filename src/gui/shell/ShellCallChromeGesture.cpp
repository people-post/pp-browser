#include "gui/shell/ShellCallChromeGesture.h"

#include "common/Diag.h"

#include <ui/base/SystemInterface.h>
#include <ui/base/Tween.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/style/Property.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace pbr {

namespace {

constexpr float kModeThresholdDp = 56.f;
constexpr int kDragDeadzonePx = 8;
constexpr float kScrollTopEpsilonPx = 1.f;
constexpr float kTapSlopDp = 10.f;
/** #166: the dropped chip's slide to its side (as the AI dot's, ShellAiDot kSnapSeconds), and the slide that replaces
 * one a press caught (it ends where it starts, on the next frame). */
constexpr float kSlideSeconds = 0.25f;
constexpr float kCutSeconds = 0.01f;
/** A slide whose end never came (the animation did not start, or no Animationend: a hidden chip) is ended by Reflow
 * this long after the drop, so the chip is anchored and its rebuild no longer held. */
constexpr double kSlideDeadlineSeconds = 0.4;
/** How often Reflow re-reads a docked chip's box (it only moves for a size change: a new window, the chip's words). */
constexpr double kReflowIntervalSeconds = 0.25;
constexpr const char* kDefaultCornerClass = "shell-call-minimized-chip--tr";

int EventMouseX(const ui::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const ui::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

/** The UI clock the element animations run on (a test's fake clock too). */
double NowSeconds() {
  ui::SystemInterface* system = ui::GetSystemInterface();
  return system ? system->GetElapsedTime() : 0.0;
}

/** Diagnostics (call.chip): dp rounded to whole numbers. */
int Dp(const float dp) {
  return static_cast<int>(std::lround(dp));
}

void SetDp(ui::Element* element, const char* property, const float dp) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1fdp", dp);
  element->SetProperty(property, buffer);
}

} // namespace

void ShellCallChromeGesture::Attach(ui::Element* root, ui::Context* context, CallChromeMode mode,
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
  root_->AddEventListener(ui::EventId::Mousedown, this);
  root_->AddEventListener(ui::EventId::Animationend, this);
  attached_ = true;
  if (mode_ == CallChromeMode::Minimized) {
    SetChipOffset(0.f, 0.f, false);
    DiagLine("call.chip").Add("what", "attach").Add("root_chip", root_->GetId() == "shell-call-minimized-chip").Emit();
  } else if (mode_ == CallChromeMode::Expanded) {
    SetOutsideTapCapture(true);
  } else {
    SetRootOffsetY(0.f, false);
  }
}

void ShellCallChromeGesture::Detach() {
  // Let go of the chip once, after the gesture is torn down (the Abort below does not tell it again).
  std::function<void()> released =
      attached_ && mode_ == CallChromeMode::Minimized ? std::move(callbacks_.on_chip_released) : nullptr;
  callbacks_.on_chip_released = nullptr;
  Abort();
  SetClickSuppress(false);
  SetOutsideTapCapture(false);
  SetDismissOwnsTopOverscroll(false);
  if (attached_ && root_) {
    root_->RemoveEventListener(ui::EventId::Mousedown, this);
    root_->RemoveEventListener(ui::EventId::Animationend, this);
  }
  root_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  arm_target_ = nullptr;
  callbacks_ = {};
  axis_lock_ = nullptr;
  attached_ = false;
  suppress_click_ = false;
  settling_ = false;
  target_.reset();
  docked_ = true;
  next_reflow_s_ = 0.0;
  reflow_area_height_dp_ = 0.f;
  layout_.reset();
  if (released) {
    released();
  }
}

void ShellCallChromeGesture::Abort() {
  AbortArm(true);
}

void ShellCallChromeGesture::SetDocumentDragCapture(bool enabled) {
  if (!document_ || document_drag_capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
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
    document_->AddEventListener(ui::EventId::Click, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Click, this, true);
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

bool ShellCallChromeGesture::ShouldIgnoreTarget(ui::Element* target) const {
  if (!target) {
    return true;
  }
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    const ui::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input" || tag == "select") {
      return true;
    }
    if (tag == "button") {
      return true;
    }
    if (node->IsClassSet("btn") || node->IsClassSet("shell-close-btn")) {
      return true;
    }
    // The phone page's "how this call got through" sheet drags itself (ShellPageSheetGesture).
    if (node->IsClassSet("shell-page-sheet") || node->IsClassSet("call-phone-sheet-scrim")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::ShouldIgnoreOutsideDismiss(ui::Element* target) const {
  if (!target) {
    return true;
  }
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    const ui::String& id = node->GetId();
    if (id == "shell-call-ring-mount" || id == "shell-dialog-mount" || id == "shell-pin-gate-mount") {
      return true;
    }
    if (node->IsClassSet("shell-overlay-chrome-mount") ||
        node->IsClassSet("shell-sheet-scrim") || node->IsClassSet("shell-scrim")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::IsUnderRoot(ui::Element* target) const {
  if (!root_ || !target) {
    return false;
  }
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node == root_) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::IsScrollRegion(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node == root_) {
      break;
    }
    if (node->IsClassSet("shell-call-immersive-roster")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::IsMemberArea(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node == root_) {
      break;
    }
    if (node->IsClassSet("call-phone-group-body") || node->IsClassSet("call-phone-strip")) {
      return true;
    }
  }
  return false;
}

bool ShellCallChromeGesture::ScrollAncestorsAtTop(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
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

bool ShellCallChromeGesture::ShouldArmImmersivePullDown(ui::Element* target) const {
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
  for (ui::Element* node = arm_target_; node; node = node->GetParentNode()) {
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
  chip_dx_dp_ = dx_dp;
  chip_dy_dp_ = dy_dp;
}

float ShellCallChromeGesture::PxToDp(const float px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? px / ratio : px;
}

bool ShellCallChromeGesture::MeasureChip(CallChipRect* chip, CallChipArea* area) const {
  ui::Element* layer = root_ ? root_->GetParentNode() : nullptr;
  if (!layer) {
    return false;
  }
  // `left` / `top` place the chip from its layer's padding edge (the layer fills the call mount).
  const ui::Vector2f origin = layer->GetAbsoluteOffset(ui::BoxArea::Padding);
  const ui::Vector2f layer_size = layer->GetBox().GetSize(ui::BoxArea::Padding);
  const ui::Vector2f at = root_->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = root_->GetBox().GetSize(ui::BoxArea::Border);
  if (layer_size.x <= 0.f || layer_size.y <= 0.f || size.y <= 0.f) {
    return false;
  }
  *chip = {PxToDp(at.x - origin.x), PxToDp(at.y - origin.y), PxToDp(size.x), PxToDp(size.y)};
  *area = {PxToDp(layer_size.x), PxToDp(layer_size.y)};
  return true;
}

void ShellCallChromeGesture::DropChip() {
  CallChipRect chip;
  CallChipArea area;
  if (!MeasureChip(&chip, &area)) {
    SetChipOffset(0.f, 0.f, true);
    return;
  }
  reflow_area_height_dp_ = area.height_dp;
  // ① Where it shows as the finger leaves it: its laid-out box moved by the drag's translate.
  const float drag_dx = chip_dx_dp_;
  const float drag_dy = chip_dy_dp_;
  chip.left_dp += chip_dx_dp_;
  chip.top_dp += chip_dy_dp_;
  // ② Pinned there by left / top (the whole slide runs on these two: nothing interpolates between left and right),
  // at its width (near an edge a shrink-to-fit box would narrow), without its corner class.
  root_->SetClass(kDefaultCornerClass, false);
  SetDp(root_, "left", chip.left_dp);
  SetDp(root_, "top", chip.top_dp);
  SetDp(root_, "width", chip.width_dp);
  root_->SetProperty("right", "auto");
  root_->SetProperty("bottom", "auto");
  // ③ Then the translate goes: in the same frame, so it does not move.
  SetChipOffset(0.f, 0.f, true);
  // ④ To the nearer side at the height it was let go, clamped into the band.
  const CallChipPlace place = CallChipDock::Release(chip, area);
  const CallChipPoint end = CallChipDock::Place(place, area, chip.width_dp, chip.height_dp);
  const ui::Tween tween(ui::Tween::Cubic, ui::Tween::Out);
  const ui::Property from_x(chip.left_dp, ui::Unit::DP);
  const ui::Property from_y(chip.top_dp, ui::Unit::DP);
  root_->Animate("left", ui::Property(end.left_dp, ui::Unit::DP), kSlideSeconds, tween, 1, false, 0.f, &from_x);
  root_->Animate("top", ui::Property(end.top_dp, ui::Unit::DP), kSlideSeconds, tween, 1, false, 0.f, &from_y);
  settling_ = true;
  slide_deadline_s_ = NowSeconds() + kSlideDeadlineSeconds;
  docked_ = false;
  target_ = place;
  // The slide's end with the heights measured now: a rebuild from here on (ShellHost) puts the chip exactly there.
  layout_ = CallChipLayout{place, end.top_dp, area.height_dp, chip.height_dp};
  {
    const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
    const float viewport_h = context_ && ratio > 0.f ? static_cast<float>(context_->GetDimensions().y) / ratio : 0.f;
    ui::Element* rebuilt = document_ ? document_->GetElementById("shell-call-minimized-chip") : nullptr;
    DiagLine("call.chip")
        .Add("what", "drop")
        .Add("left", Dp(chip.left_dp))
        .Add("top", Dp(chip.top_dp))
        .Add("w", Dp(chip.width_dp))
        .Add("h", Dp(chip.height_dp))
        .Add("drag_dx", Dp(drag_dx))
        .Add("drag_dy", Dp(drag_dy))
        .Add("layer_w", Dp(area.width_dp))
        .Add("layer_h", Dp(area.height_dp))
        .Add("viewport_h", Dp(viewport_h))
        .Add("side", place.side)
        .Add("y", place.y_permille)
        .Add("target_left", Dp(end.left_dp))
        .Add("target_top", Dp(end.top_dp))
        .Add("root_chip", root_->GetId() == "shell-call-minimized-chip")
        .Add("root_is_rebuilt", rebuilt == root_)
        .Emit();
  }
  // ⑤ comes when the slide ends (FinishSlide). ⑥ The place goes to the call state now, so a restore mid-slide
  // still finds it there; until the state has it, and while the slide runs, ShellHost does not rebuild the chip.
  if (callbacks_.on_chip_place) {
    callbacks_.on_chip_place(place.side, place.y_permille);
  }
}

void ShellCallChromeGesture::FreezeChip() {
  settling_ = false;
  CallChipRect chip;
  CallChipArea area;
  if (!root_ || !MeasureChip(&chip, &area)) {
    return;
  }
  DiagLine("call.chip").Add("what", "freeze").Add("left", Dp(chip.left_dp)).Add("top", Dp(chip.top_dp)).Emit();
  // The slide's left / top as last laid out: a new "slide" from there to there replaces it (an element keeps one
  // animation per property), so the old one moves it no more; the drag's translate follows the finger from here.
  const ui::Tween tween(ui::Tween::Cubic, ui::Tween::Out);
  const ui::Property x(chip.left_dp, ui::Unit::DP);
  const ui::Property y(chip.top_dp, ui::Unit::DP);
  root_->Animate("left", x, kCutSeconds, tween, 1, false, 0.f, &x);
  root_->Animate("top", y, kCutSeconds, tween, 1, false, 0.f, &y);
  SetDp(root_, "left", chip.left_dp);
  SetDp(root_, "top", chip.top_dp);
  docked_ = false;
}

void ShellCallChromeGesture::FinishSlide() {
  settling_ = false;
  if (!root_ || !target_) {
    return;
  }
  DiagLine("call.chip").Add("what", "settle").Emit();
  // ⑤ There: anchored on its own side, so a chip that grows later grows away from the edge, never past it.
  DockChip(*target_);
  NotifyChipReleased();
}

void ShellCallChromeGesture::NotifyChipReleased() {
  if (callbacks_.on_chip_released) {
    callbacks_.on_chip_released();
  }
}

void ShellCallChromeGesture::DockChip(const CallChipPlace& place) {
  CallChipRect chip;
  CallChipArea area;
  if (!root_ || !MeasureChip(&chip, &area)) {
    return;
  }
  reflow_area_height_dp_ = area.height_dp;
  const CallChipPoint at = CallChipDock::Place(place, area, chip.width_dp, chip.height_dp);
  root_->SetClass(kDefaultCornerClass, false);
  root_->RemoveProperty("width");
  if (place.side == CallChipDock::kSideLeft) {
    SetDp(root_, "left", CallChipDock::kEdgeDp);
    root_->SetProperty("right", "auto");
  } else {
    SetDp(root_, "right", CallChipDock::kEdgeDp);
    root_->SetProperty("left", "auto");
  }
  SetDp(root_, "top", at.top_dp);
  root_->SetProperty("bottom", "auto");
  docked_ = true;
  layout_ = CallChipLayout{place, at.top_dp, area.height_dp, chip.height_dp};
  DiagLine line("call.chip");
  line.Add("what", "dock").Add("top", Dp(at.top_dp)).Add("laid_top", Dp(chip.top_dp));
  if (place.side == CallChipDock::kSideLeft) {
    line.Add("anchor", "left");
  } else {
    line.Add("anchor", "right");
  }
  line.Add("settling", settling_).Emit();
}

bool ShellCallChromeGesture::HoldsChip(const CallInProgressState& call) const {
  if (!attached_ || !root_ || mode_ != CallChromeMode::Minimized) {
    return false;
  }
  if (tracking_ || settling_) {
    return true;
  }
  return target_ && (call.minimized_side != target_->side || call.minimized_y_permille != target_->y_permille);
}

ui::Element* CallChromeGestureElement(ui::ElementDocument* doc, const CallChromeMode mode) {
  if (!doc) {
    return nullptr;
  }
  if (mode == CallChromeMode::Minimized) {
    if (ui::Element* chip = doc->GetElementById("shell-call-minimized-chip")) {
      return chip;
    }
  }
  return doc->GetElementById("shell-call-chrome-root");
}

void RememberCallChipLayout(const ShellCallChromeGesture& gesture, CallChipLayout& into) {
  if (const std::optional<CallChipLayout>& layout = gesture.Layout()) {
    into = *layout;
  }
}

bool KeepMinimizedChipOnRemount(const CallInProgressState& call, const ui::Element* chip,
                                const ShellCallChromeGesture& gesture) {
  return call.active && call.mode == CallChromeMode::Minimized && gesture.AttachedTo(chip) && gesture.HoldsChip(call);
}

void ShellCallChromeGesture::Reflow(const CallInProgressState& call) {
  if (!attached_ || !root_ || mode_ != CallChromeMode::Minimized || tracking_) {
    return;
  }
  const double now = NowSeconds();
  if (settling_) {
    if (now < slide_deadline_s_) {
      return;
    }
    FinishSlide(); // its Animationend never came
  }
  const CallChipPlace place = target_ ? *target_ : CallChipPlace{call.minimized_side, call.minimized_y_permille};
  if (place.side == CallChipDock::kSideDefault) {
    return;
  }
  // A docked chip that showed in place is looked at again only every kReflowIntervalSeconds, or for a new place.
  if (docked_ && place == reflowed_place_ && now < next_reflow_s_) {
    return;
  }
  CallChipRect chip;
  CallChipArea area;
  if (!MeasureChip(&chip, &area)) {
    return; // not laid out yet (a chip just mounted): the next frame looks again
  }
  // The frame after a rebuild lays the layer out at a transient height (iPhone trace 2026-11-08: the band collapsed,
  // the chip went to 12dp and 250ms later back to its place). A chip resting in the band is only moved for a layer
  // height seen on two frames in a row, and no chip is moved into a band with no room in it. One not yet in the band
  // (rebuilt with no top) is placed at once: a frame at the wrong spot would show.
  const bool band_collapsed = CallChipDock::MaxTop(area, chip.height_dp) <= CallChipDock::MinTop();
  const bool area_settled = std::fabs(area.height_dp - reflow_area_height_dp_) < 0.5f;
  reflow_area_height_dp_ = area.height_dp;
  const bool resting = docked_ && chip.top_dp >= CallChipDock::MinTop() - 0.5f;
  if (band_collapsed || (resting && !area_settled)) {
    return; // the next frame measures again
  }
  next_reflow_s_ = now + kReflowIntervalSeconds;
  reflowed_place_ = place;
  const float top = CallChipDock::Place(place, area, chip.width_dp, chip.height_dp).top_dp;
  if (!docked_ || std::fabs(top - chip.top_dp) >= 0.5f) {
    DiagLine("call.chip")
        .Add("what", "reflow")
        .Add("top_before", Dp(chip.top_dp))
        .Add("top_after", Dp(top))
        .Add("layer_h", Dp(area.height_dp))
        .Add("chip_h", Dp(chip.height_dp))
        .Add("docked", docked_)
        .Add("animated", false)
        .Emit();
    DockChip(place);
  }
}

void ShellCallChromeGesture::BeginArm(int x_px, int y_px, ui::Element* target) {
  tracking_ = true;
  dragging_ = false;
  arm_target_ = target;
  drag_start_x_px_ = x_px;
  drag_start_y_px_ = y_px;
  drag_last_x_px_ = x_px;
  drag_last_y_px_ = y_px;
  if (mode_ == CallChromeMode::Minimized) {
    CallChipRect chip;
    CallChipArea area;
    const bool laid_out = MeasureChip(&chip, &area);
    DiagLine("call.chip")
        .Add("what", "press")
        .Add("settling", settling_)
        .Add("docked", docked_)
        .Add("laid_out", laid_out)
        .Add("left", Dp(chip.left_dp))
        .Add("top", Dp(chip.top_dp))
        .Emit();
  }
  if (mode_ == CallChromeMode::Immersive && !members_drag_) {
    SetDismissOwnsTopOverscroll(true);
  }
  SetDocumentDragCapture(true);
}

void ShellCallChromeGesture::AbortArm(bool unlock_axis) {
  members_drag_ = false;
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
  if (mode_ == CallChromeMode::Minimized) {
    NotifyChipReleased(); // the press is over with no drop: a rebuild held for it runs
  }
}

void ShellCallChromeGesture::UpdateDrag(int x_px, int y_px, ui::Event& /*event*/) {
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

  // Expanded, and a drag over the phone group's members: only a drag (no click); never a mode change.
  if (mode_ == CallChromeMode::Expanded || members_drag_) {
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
  const bool members_drag = members_drag_;
  const int dy_px = drag_last_y_px_ - drag_start_y_px_;
  const float dy_dp = PixelDeltaToDp(dy_px);

  tracking_ = false;
  dragging_ = false;
  members_drag_ = false;
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
      // The press is over with no drop: a rebuild held for it runs (it merges with the restore's own remount).
      NotifyChipReleased();
      if (callbacks_.on_restore) {
        callbacks_.on_restore();
      }
      return;
    }
    unlock();
    DropChip();
    return;
  }

  if (mode_ == CallChromeMode::Immersive) {
    SetRootOffsetY(0.f, true);
    unlock();
    if (was_dragging && !members_drag && dy_dp >= kModeThresholdDp && callbacks_.on_expand) {
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

void ShellCallChromeGesture::ProcessEvent(ui::Event& event) {
  switch (event.GetId()) {
  case ui::EventId::Mousedown: {
    if (tracking_) {
      return;
    }
    if (suppress_click_) {
      // The click the last drag held back never came (a touch that scrolled sends none): this press is a new tap.
      suppress_click_ = false;
      SetClickSuppress(false);
    }
    ui::Element* target = event.GetTargetElement();
    if (mode_ == CallChromeMode::Minimized) {
      if (!IsUnderRoot(target) || ShouldIgnoreTarget(target)) {
        return;
      }
      if (settling_) {
        FreezeChip(); // caught mid-slide: it stops under the finger, then follows it
      }
      BeginArm(EventMouseX(event), EventMouseY(event), target);
      return;
    }
    if (mode_ == CallChromeMode::Immersive) {
      // The phone group page's members (the grid, the spotlit one, the strip, the gaps between them): a swipe there
      // looks through them and must never shrink the call (#94); a tap still spotlights the one tapped.
      members_drag_ = IsUnderRoot(target) && !ShouldIgnoreTarget(target) && IsMemberArea(target);
      if (!members_drag_ && !ShouldArmImmersivePullDown(target)) {
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
  case ui::EventId::Mousemove:
    if (tracking_) {
      UpdateDrag(EventMouseX(event), EventMouseY(event), event);
    }
    break;
  case ui::EventId::Mouseup:
    if (tracking_) {
      EndDrag();
    }
    break;
  case ui::EventId::Animationend:
    // The slide's left and top end in the same frame: the first one anchors the chip on its side.
    if (settling_ && event.GetTargetElement() == root_) {
      const ui::String property = event.GetParameter<ui::String>("property", "");
      if (property == "left" || property == "top") {
        FinishSlide();
      }
    }
    break;
  case ui::EventId::Click:
    if (mode_ == CallChromeMode::Expanded && outside_tap_capture_) {
      ui::Element* target = event.GetTargetElement();
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
