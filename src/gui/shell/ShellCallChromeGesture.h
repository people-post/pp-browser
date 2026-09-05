#pragma once

#include "domain/ui/ShellTypes.h"
#include "gui/shell/ShellGestureAxis.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Types.h>

#include <functional>

namespace Rml {
class Context;
class Event;
class ElementDocument;
}

namespace pbr {

/**
 * Call chrome gestures (V031).
 *
 * Expanded: tap non-button chrome → Immersive; tap outside chrome → Minimized.
 * Immersive: pull down on non-button chrome, or pull down in roster at scroll top → Expanded.
 * Minimized: tap → restore; drag → move / corner snap. No swipe-to-mode.
 */
class ShellCallChromeGesture : public Rml::EventListener {
public:
  struct Callbacks {
    std::function<void()> on_minimize;
    std::function<void()> on_immersive;
    std::function<void()> on_expand;
    std::function<void()> on_restore;
    std::function<void(int corner)> on_chip_corner;
  };

  void Attach(Rml::Element* root, Rml::Context* context, CallChromeMode mode, Callbacks callbacks,
              ShellGestureAxisLock* axis_lock = nullptr);
  void Detach();
  void Abort();

  void ProcessEvent(Rml::Event& event) override;

private:
  bool ShouldIgnoreTarget(Rml::Element* target) const;
  bool ShouldIgnoreOutsideDismiss(Rml::Element* target) const;
  bool IsUnderRoot(Rml::Element* target) const;
  bool IsScrollRegion(Rml::Element* target) const;
  bool ScrollAncestorsAtTop(Rml::Element* target) const;
  bool ShouldArmImmersivePullDown(Rml::Element* target) const;
  void PinScrollAncestorsAtTop();
  void BeginArm(int x_px, int y_px, Rml::Element* target);
  void AbortArm(bool unlock_axis);
  void UpdateDrag(int x_px, int y_px, Rml::Event& event);
  void EndDrag();
  void SetRootOffsetY(float dy_dp, bool animate);
  void SetChipOffset(float dx_dp, float dy_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  void SetOutsideTapCapture(bool enabled);
  void SetClickSuppress(bool enabled);
  void UpdateDocumentClickCapture();
  void SetDismissOwnsTopOverscroll(bool owns);
  float PixelDeltaToDp(int delta_px) const;
  int SnapCornerFromChipCenter() const;
  void ApplyCornerClass(int corner);

  Rml::Element* root_ = nullptr;
  Rml::ElementDocument* document_ = nullptr;
  Rml::Context* context_ = nullptr;
  Rml::Element* arm_target_ = nullptr;
  CallChromeMode mode_ = CallChromeMode::Expanded;
  Callbacks callbacks_;
  ShellGestureAxisLock* axis_lock_ = nullptr;
  bool attached_ = false;
  bool document_drag_capture_ = false;
  bool document_click_capture_ = false;
  bool outside_tap_capture_ = false;
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
