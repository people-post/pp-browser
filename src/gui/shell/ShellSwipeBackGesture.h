#pragma once

#include "domain/ui/ShellGestureAxis.h"

#include <ui/dom/Element.h>
#include <ui/dom/EventListener.h>
#include <ui/base/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Event;
class ElementDocument;
}

namespace pbr {

/** Horizontal edge / chrome swipe-back for drill-down surfaces. */
class ShellSwipeBackGesture : public ui::EventListener {
public:
  using DismissCallback = std::function<void()>;

  struct AttachOptions {
    float width_dp_fallback = 0.f;
    /** Class names that count as chrome (back starts from anywhere on these). */
    std::vector<std::string> chrome_classes = {"shell-back-btn"};
    const char* dragging_class = "shell-swipe-dragging";
    ShellGestureAxisLock* axis_lock = nullptr;
    bool require_edge = true;
    /**
     * Element that slides horizontally. Defaults to the listen surface when null.
     * Use when the listener is on a parent (e.g. settings pane) but only the detail
     * panel should translate.
     */
    ui::Element* transform_target = nullptr;
    /**
     * Optional: only start when the event target is under this element (or chrome).
     * Useful when listening on a parent that also hosts non-drill-down chrome.
     */
    ui::Element* content_root = nullptr;
  };

  void Attach(ui::Element* listen_surface, ui::Context* context, AttachOptions options,
              DismissCallback on_dismiss);
  void Detach();
  void Abort();

  void ProcessEvent(ui::Event& event) override;

private:
  bool ShouldIgnoreTarget(ui::Element* target) const;
  bool ShouldStartSwipe(ui::Element* target, int x_px) const;
  bool IsChromeRegion(ui::Element* target) const;
  bool IsUnder(ui::Element* ancestor, ui::Element* target) const;
  ui::Element* TransformTarget() const;
  void BeginDrag(int x_px, int y_px, bool from_edge);
  void UpdateDrag(int x_px, int y_px, ui::Event& event);
  void EndDrag();
  void SetSurfaceOffset(float dx_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  float PixelDeltaToDp(int delta_px) const;
  float ResolveSurfaceWidthDp() const;

  ui::Element* surface_ = nullptr;
  ui::Element* transform_target_ = nullptr;
  ui::Element* content_root_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  AttachOptions options_;
  DismissCallback on_dismiss_;
  bool attached_ = false;
  bool document_drag_capture_ = false;
  bool tracking_ = false;
  bool dragging_ = false;
  bool from_edge_ = false;
  int drag_start_x_px_ = 0;
  int drag_start_y_px_ = 0;
  int drag_last_x_px_ = 0;
  int drag_last_y_px_ = 0;
};

} // namespace pbr
