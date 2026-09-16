#pragma once

#include "gui/shell/ShellGestureAxis.h"

#include <ui/dom/Element.h>
#include <ui/dom/EventListener.h>
#include <ui/base/Types.h>

#include <functional>

namespace ui {
class Context;
class Event;
class ElementDocument;
}

namespace pbr {

class ShellBottomSheetGesture : public ui::EventListener {
public:
  using DismissCallback = std::function<void()>;

  void Attach(ui::Element* sheet, ui::Context* context, float sheet_height_dp, DismissCallback on_dismiss,
              ShellGestureAxisLock* axis_lock = nullptr);
  void Detach();
  void Abort();

  void ProcessEvent(ui::Event& event) override;

private:
  bool ShouldIgnoreTarget(ui::Element* target) const;
  bool ShouldStartSwipe(ui::Element* target) const;
  bool IsUnderSheet(ui::Element* target) const;
  bool IsChromeRegion(ui::Element* target) const;
  bool ScrollAncestorsAtTop(ui::Element* target) const;
  void PinScrollAncestorsAtTop();
  void BeginArm(int x_px, int y_px, ui::Element* target);
  void AbortArm(bool unlock_axis);
  void UpdateDrag(int x_px, int y_px, ui::Event& event);
  void EndDrag();
  void SetSheetOffset(float dy_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  void SetClickSuppress(bool enabled);
  void SetDismissOwnsTopOverscroll(bool owns);
  float PixelDeltaToDp(int delta_px) const;
  float ResolveSheetHeightDp() const;

  ui::Element* sheet_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  ui::Element* arm_target_ = nullptr;
  float sheet_height_dp_ = 0.f;
  DismissCallback on_dismiss_;
  ShellGestureAxisLock* axis_lock_ = nullptr;
  bool attached_ = false;
  bool document_drag_capture_ = false;
  bool click_suppress_listener_ = false;
  bool suppress_click_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  bool dismiss_owns_top_overscroll_ = false;
  int drag_start_x_px_ = 0;
  int drag_start_y_px_ = 0;
  int drag_last_x_px_ = 0;
  int drag_last_y_px_ = 0;
};

} // namespace pbr
