#pragma once

#include "lib/ui/CallChipDock.h"
#include "lib/ui/ShellTypes.h"
#include "lib/ui/ShellGestureAxis.h"

#include <ui/dom/Element.h>
#include <ui/dom/EventListener.h>
#include <ui/base/Types.h>

#include <functional>
#include <optional>

namespace ui {
class Context;
class Event;
class ElementDocument;
}

namespace pbr {

/**
 * Call chrome gestures (V031).
 *
 * Expanded: tap non-button chrome → Immersive; tap outside chrome → Minimized.
 * Immersive: pull down on non-button chrome, or pull down in roster at scroll top → Expanded. Not over the phone
 * group page's members (grid, spotlight, strip): a drag there never changes the mode (#94).
 * Minimized: tap → restore; drag → move; let go → it slides to the nearer side at the height it was dropped
 * (#166, CallChipDock; like the phone AI dot). No swipe-to-mode.
 */
class ShellCallChromeGesture : public ui::EventListener {
public:
  struct Callbacks {
    std::function<void()> on_minimize;
    std::function<void()> on_immersive;
    std::function<void()> on_expand;
    std::function<void()> on_restore;
    /** #166: the chip was dropped; where it now rests (CallChipDock: side 1 left / 2 right, height 0..1000). */
    std::function<void(int side, int y_permille)> on_chip_place;
    /** #166: the chip's slide to its side has ended and it is anchored there. */
    std::function<void()> on_chip_settled;
  };

  void Attach(ui::Element* root, ui::Context* context, CallChromeMode mode, Callbacks callbacks,
              ShellGestureAxisLock* axis_lock = nullptr);
  void Detach();
  void Abort();

  void ProcessEvent(ui::Event& event) override;

  /** Attached to `element` (ShellHost: is the chip on screen the one this gesture moves?). */
  bool AttachedTo(const ui::Element* element) const { return attached_ && element != nullptr && root_ == element; }
  /** #166: the chip is sliding from where it was dropped to its side. */
  bool IsSettling() const { return settling_; }
  /** #166: where this gesture last dropped the chip (nullopt: not since it was attached). */
  const std::optional<CallChipPlace>& DropTarget() const { return target_; }
  /**
   * #166: the chip's position is still this gesture's — pressed or dragged, sliding to its side, or dropped at a place
   * `call` does not have yet. Rebuilding the chip from `call` now would put it back where it was: ShellHost only
   * dirties its text until this is false.
   */
  bool HoldsChip(const CallInProgressState& call) const;
  /** #166: where this gesture last put a dropped chip (the drop's slide end, the dock, a correction); nullopt: none. */
  const std::optional<CallChipLayout>& Layout() const { return layout_; }
  /** #166: once a frame (before the layout): keeps a dropped chip on its side and in its band as sizes change. Its
   * corrections are put in place at once (never slid). */
  void Reflow(const CallInProgressState& call);

private:
  bool ShouldIgnoreTarget(ui::Element* target) const;
  bool ShouldIgnoreOutsideDismiss(ui::Element* target) const;
  bool IsUnderRoot(ui::Element* target) const;
  bool IsScrollRegion(ui::Element* target) const;
  bool IsMemberArea(ui::Element* target) const;
  bool ScrollAncestorsAtTop(ui::Element* target) const;
  bool ShouldArmImmersivePullDown(ui::Element* target) const;
  void PinScrollAncestorsAtTop();
  void BeginArm(int x_px, int y_px, ui::Element* target);
  void AbortArm(bool unlock_axis);
  void UpdateDrag(int x_px, int y_px, ui::Event& event);
  void EndDrag();
  void SetRootOffsetY(float dy_dp, bool animate);
  void SetChipOffset(float dx_dp, float dy_dp, bool animate);
  void SetDocumentDragCapture(bool enabled);
  void SetOutsideTapCapture(bool enabled);
  void SetClickSuppress(bool enabled);
  void UpdateDocumentClickCapture();
  void SetDismissOwnsTopOverscroll(bool owns);
  float PixelDeltaToDp(int delta_px) const;
  float PxToDp(float px) const;
  /** The chip's laid-out box (without the drag's translate) and its layer, in dp; false when not laid out. */
  bool MeasureChip(CallChipRect* chip, CallChipArea* area) const;
  /** Lets go of a dragged chip: pins it where it shows, then slides it to its side (CallChipDock::Release). */
  void DropChip();
  /** A press caught the chip mid-slide: the slide stops and the chip stays where it shows. */
  void FreezeChip();
  /** The slide reached the side: anchor the chip there (right: `right`, left: `left`), at its band height. */
  void FinishSlide();
  void DockChip(const CallChipPlace& place);

  ui::Element* root_ = nullptr;
  ui::ElementDocument* document_ = nullptr;
  ui::Context* context_ = nullptr;
  ui::Element* arm_target_ = nullptr;
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
  /** This press began over the phone group's members: tracked only to hold back the click after a drag. */
  bool members_drag_ = false;
  bool dismiss_owns_top_overscroll_ = false;
  int drag_start_x_px_ = 0;
  int drag_start_y_px_ = 0;
  int drag_last_x_px_ = 0;
  int drag_last_y_px_ = 0;
  /** The chip's drag translate as last set (dp). */
  float chip_dx_dp_ = 0.f;
  float chip_dy_dp_ = 0.f;
  /** #166: sliding to its side after a drop; the place it was dropped at; anchored on its side (not pinned by a slide
   * or a press that stopped one). */
  bool settling_ = false;
  std::optional<CallChipPlace> target_;
  bool docked_ = true;
  /** #166: when Reflow ends a slide whose Animationend never came; when it next re-reads a docked chip, and for which
   * place it last did (UI clock, seconds). */
  double slide_deadline_s_ = 0.0;
  double next_reflow_s_ = 0.0;
  CallChipPlace reflowed_place_;
  std::optional<CallChipLayout> layout_;
};

/**
 * #166: a call chrome remount leaves the minimised chip `chip` (on screen now) as it is: the call is still minimised and
 * `gesture`, attached to that chip, still owns its position (HoldsChip). ShellHost::RemountCallChromeNow then only
 * dirties the chip's (bound) text and rebuilds it when the slide has ended and `call` has the dropped place.
 */
/**
 * The element the call chrome gesture attaches to (ShellHost::AttachCallChromeGesture): minimised, the chip itself —
 * never its full-screen layer, which takes no touches and is what the chip is placed in (#166: the chip's measurement,
 * its drop, its slide and the remount hold all act on this element); otherwise the call chrome root.
 */
ui::Element* CallChromeGestureElement(ui::ElementDocument* doc, CallChromeMode mode);

/** #166: copies `gesture`'s last chip layout (if any) over `into` (ShellHost, before it renders the chip). */
void RememberCallChipLayout(const ShellCallChromeGesture& gesture, CallChipLayout& into);

bool KeepMinimizedChipOnRemount(const CallInProgressState& call, const ui::Element* chip,
                                const ShellCallChromeGesture& gesture);

} // namespace pbr
