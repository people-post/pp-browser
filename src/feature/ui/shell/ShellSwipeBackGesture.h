#pragma once

#include "feature/ui/shell/ShellGestureAxis.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace Rml {
class Context;
class Event;
class ElementDocument;
}

namespace pbr {

/** Horizontal edge / chrome swipe-back for drill-down surfaces. */
class ShellSwipeBackGesture : public Rml::EventListener {
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
    Rml::Element* transform_target = nullptr;
    /**
     * Optional: only start when the event target is under this element (or chrome).
     * Useful when listening on a parent that also hosts non-drill-down chrome.
     */
    Rml::Element* content_root = nullptr;
  };

  void Attach(Rml::Element* listen_surface, Rml::Context* context, AttachOptions options,
              DismissCallback on_dismiss);
  void Detach();
  void Abort();

  void ProcessEvent(Rml::Event& event) override;

private:
  bool ShouldIgnoreTarget(Rml::Element* target) const;
  bool ShouldStartSwipe(Rml::Element* target, int x_px) const;
  bool IsChromeRegion(Rml::Element* target) const;
  bool IsUnder(Rml::Element* ancestor, Rml::Element* target) const;
  Rml::Element* TransformTarget() const;
  void BeginDrag(int x_px, int y_px, bool from_edge);
  void UpdateDrag(int x_px, int y_px, Rml::Event& event);
  void EndDrag();
  void SetSurfaceOffset(float dx_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  float PixelDeltaToDp(int delta_px) const;
  float ResolveSurfaceWidthDp() const;

  Rml::Element* surface_ = nullptr;
  Rml::Element* transform_target_ = nullptr;
  Rml::Element* content_root_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Context* context_ = nullptr;
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
