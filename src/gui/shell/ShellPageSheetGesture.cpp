#include "gui/shell/ShellPageSheetGesture.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace pbr {

namespace {

constexpr int kDeadzonePx = 8;
constexpr float kScrollTopEpsilonPx = 1.f;
constexpr const char* kDraggingClass = "shell-swipe-dragging";

double NowMs() {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

void ShellPageSheetGesture::Attach(ui::Element* sheet, ui::Context* context, std::function<void()> on_dismiss,
                                   Options options) {
  Detach();
  if (!sheet || !context) {
    return;
  }
  sheet_ = sheet;
  document_ = sheet->GetOwnerDocument();
  context_ = context;
  on_dismiss_ = std::move(on_dismiss);
  options_ = std::move(options);
  sheet_->AddEventListener(ui::EventId::Mousedown, this);
  sheet_->AddEventListener(ui::EventId::Click, this, true);
}

void ShellPageSheetGesture::Detach() {
  SetDocumentCapture(false);
  if (tracking_ && context_) {
    context_->SetScrollOverscrollEdges(true, true, true, true);
  }
  if (sheet_) {
    sheet_->RemoveEventListener(ui::EventId::Mousedown, this);
    sheet_->RemoveEventListener(ui::EventId::Click, this, true);
  }
  sheet_ = nullptr;
  press_target_ = nullptr;
  document_ = nullptr;
  context_ = nullptr;
  on_dismiss_ = {};
  options_ = {};
  may_drag_up_ = false;
  tracking_ = false;
  dragging_ = false;
  swallow_click_ = false;
}

float ShellPageSheetGesture::ToDp(const int px) const {
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  return ratio > 0.f ? static_cast<float>(px) / ratio : static_cast<float>(px);
}

bool ShellPageSheetGesture::ShouldIgnoreTarget(ui::Element* target) const {
  if (!target) {
    return true;
  }
  for (ui::Element* node = target; node && node != sheet_; node = node->GetParentNode()) {
    const ui::String& tag = node->GetTagName();
    if (tag == "input" || tag == "textarea" || tag == "select" || tag == "button") {
      return true;
    }
  }
  return false;
}

bool ShellPageSheetGesture::ScrolledToTop(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f && node->GetScrollTop() > kScrollTopEpsilonPx) {
      return false;
    }
    if (node == sheet_) {
      break;
    }
  }
  return true;
}

bool ShellPageSheetGesture::InScrollingContent(ui::Element* target) const {
  for (ui::Element* node = target; node && node != sheet_; node = node->GetParentNode()) {
    if (node->GetScrollHeight() > node->GetClientHeight() + 0.5f) {
      return true;
    }
  }
  return false;
}

void ShellPageSheetGesture::SetOffset(const float dy_dp, const bool animate) {
  if (!sheet_) {
    return;
  }
  sheet_->SetClass("shell-page-sheet-enter", false);
  sheet_->SetClass(kDraggingClass, !animate);
  char buffer[64];
  if (dy_dp < 0.f && may_drag_up_) {
    // Up: the top edge itself moves (a transform would lift the bottom off the screen).
    const float top_dp = std::max(PageSheetModel::kFullTopDp, press_top_dp_ + dy_dp);
    std::snprintf(buffer, sizeof(buffer), "%.1fdp", top_dp);
    sheet_->SetProperty("top", buffer);
    sheet_->SetProperty("transform", "translateY(0dp)");
    return;
  }
  sheet_->RemoveProperty("top");
  std::snprintf(buffer, sizeof(buffer), "translateY(%.1fdp)", dy_dp < 0.f ? 0.f : dy_dp);
  sheet_->SetProperty("transform", buffer);
}

void ShellPageSheetGesture::SetDocumentCapture(const bool enabled) {
  if (!document_ || document_capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  document_capture_ = enabled;
}

void ShellPageSheetGesture::Release() {
  SetDocumentCapture(false);
  if (context_) {
    context_->SetScrollOverscrollEdges(true, true, true, true);
  }
  const bool was_dragging = dragging_;
  tracking_ = false;
  dragging_ = false;
  if (!was_dragging) {
    return;
  }
  // A drag that began on a row must not also tap it when the finger lifts.
  swallow_click_ = true;
  swallow_since_ = std::chrono::steady_clock::now();
  const float drag_dp = ToDp(last_y_px_ - start_y_px_);
  const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
  const auto to_dp = [ratio](const float px) { return ratio > 0.f ? px / ratio : px; };
  const float height_dp = to_dp(sheet_->GetBox().GetSize(ui::BoxArea::Border).y);
  // The stops are shares of the layer the sheet stands in (its containing block).
  ui::Element* layer = sheet_->GetParentNode();
  const float layer_height_dp = layer ? to_dp(layer->GetClientHeight()) : height_dp;
  const PageSheetModel::Release release =
      PageSheetModel::OnRelease(press_detent_, drag_dp, layer_height_dp, velocity_.AtRelease(NowMs()));
  if (release == PageSheetModel::Release::Dismiss && on_dismiss_) {
    sheet_->RemoveProperty("top");
    SetOffset(height_dp, true);
    auto dismiss = on_dismiss_;
    dismiss(); // may detach this gesture (the shell remounts)
    return;
  }
  if (release != PageSheetModel::Release::Stay && release != PageSheetModel::Release::Dismiss && options_.on_detent) {
    // The stop's class moves the top edge (with its transition) from where the finger left it.
    options_.on_detent(release == PageSheetModel::Release::ToFull    ? SheetDetent::Full
                       : release == PageSheetModel::Release::ToLarge ? SheetDetent::Large
                       : release == PageSheetModel::Release::ToHalf  ? SheetDetent::Half
                                                                     : SheetDetent::Small);
  }
  sheet_->SetClass(kDraggingClass, false);
  sheet_->RemoveProperty("top");
  sheet_->SetProperty("transform", "translateY(0dp)");
}

void ShellPageSheetGesture::ProcessEvent(ui::Event& event) {
  if (!sheet_) {
    return;
  }
  switch (event.GetId()) {
    case ui::EventId::Click:
      if (swallow_click_) {
        const bool same_press = std::chrono::steady_clock::now() - swallow_since_ < std::chrono::milliseconds(300);
        swallow_click_ = false;
        if (same_press) {
          event.StopPropagation();
        }
      }
      break;
    case ui::EventId::Mousedown: {
      swallow_click_ = false;
      ui::Element* target = event.GetTargetElement();
      if (ShouldIgnoreTarget(target)) {
        return;
      }
      press_detent_ = options_.shown_detent ? options_.shown_detent() : SheetDetent::Full;
      may_drag_up_ = press_detent_ != SheetDetent::Full && !InScrollingContent(target);
      if (!may_drag_up_ && !ScrolledToTop(target)) {
        return;
      }
      {
        const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
        const float top_px = sheet_->GetOffsetTop();
        press_top_dp_ = ratio > 0.f ? top_px / ratio : top_px;
      }
      press_target_ = target;
      tracking_ = true;
      dragging_ = false;
      start_x_px_ = event.GetParameter<int>("mouse_x", 0);
      start_y_px_ = event.GetParameter<int>("mouse_y", 0);
      last_y_px_ = start_y_px_;
      velocity_.Reset();
      velocity_.Add(NowMs(), 0.f);
      SetDocumentCapture(true);
      break;
    }
    case ui::EventId::Mousemove: {
      if (!tracking_) {
        return;
      }
      const int x = event.GetParameter<int>("mouse_x", 0);
      const int y = event.GetParameter<int>("mouse_y", 0);
      const int dx = x - start_x_px_;
      const int dy = y - start_y_px_;
      if (!dragging_) {
        if (std::abs(dx) <= kDeadzonePx && std::abs(dy) <= kDeadzonePx) {
          return;
        }
        const bool up_ok = dy < 0 && may_drag_up_;
        if ((dy <= 0 && !up_ok) || std::abs(dx) > std::abs(dy) || (dy > 0 && !ScrolledToTop(press_target_))) {
          // Up or sideways first: the content scrolls / handles it.
          tracking_ = false;
          SetDocumentCapture(false);
          return;
        }
        dragging_ = true;
        if (context_) {
          // The list must not rubber-band down while the sheet itself follows the finger.
          context_->SetScrollOverscrollEdges(true, true, false, true);
          context_->ClearScrollOverscroll();
        }
      }
      last_y_px_ = y;
      velocity_.Add(NowMs(), ToDp(dy));
      SetOffset(ToDp(dy), false);
      event.StopPropagation();
      break;
    }
    case ui::EventId::Mouseup:
      if (tracking_) {
        Release();
      }
      break;
    default:
      break;
  }
}

} // namespace pbr
