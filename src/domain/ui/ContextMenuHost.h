#pragma once

#include <ui/dom/Event.h>
#include <ui/dom/EventListener.h>
#include <ui/base/Types.h>
#include <ui/base/Vector2.h>

#include <functional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
}

namespace pbr {

struct ContextMenuAction {
  std::string id;
  std::string label;
  std::function<bool()> enabled;
  std::function<void()> run;
  /// Optional asset-relative SVG path (e.g. "../icons/trash.svg"). Empty = text only.
  std::string icon;
  /// When true, menu item uses danger styling (destructive actions).
  bool danger = false;
  /// When true, show a selected/checkmark affordance (pickers).
  bool selected = false;
};

struct ContextMenuRequest {
  ui::Vector2i position;
  ui::Element* target = nullptr;
  ui::Context* context = nullptr;
};

/** Anchor a float menu just below an element (left-aligned). */
ui::Vector2i MenuPositionBelow(ui::Element* element, float gap_px = 4.f);
/** Anchor a float menu below an element, right-aligned to ~menu_min_width_px. */
ui::Vector2i MenuPositionBelowRightAligned(ui::Element* element, float menu_min_width_px = 180.f,
                                            float gap_px = 4.f);
/** Resolve current/target element from an event, then MenuPositionBelow. */
ui::Vector2i MenuPositionBelowEvent(ui::Event& ev, float gap_px = 4.f);
/** Resolve current/target element from an event, then MenuPositionBelowRightAligned. */
ui::Vector2i MenuPositionBelowRightAlignedEvent(ui::Event& ev, float menu_min_width_px = 180.f,
                                                 float gap_px = 4.f);

class ContextMenuHost : public ui::EventListener {
public:
  static ContextMenuHost& Instance();

  void Install(ui::Context* context);
  /// Compact layout uses a bottom action sheet for ShowActions; floats stay clamped.
  void SetCompactLayout(bool compact);
  void RegisterProvider(std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> provider);
  void ShowAt(const ContextMenuRequest& request);
  /// Show an explicit action list (no copy/select/paste text actions).
  void ShowActions(ui::Vector2i position, std::vector<ContextMenuAction> actions);
  void Dismiss();
  bool IsOpen() const { return layer_ != nullptr; }
  bool HandleDismiss();
  /// Apply a deferred dismiss after the current pointer event finishes (safe DOM teardown).
  void Update();

  void OnLongPress(ui::Vector2i position, ui::Element* target);
  bool OnContextPointer(ui::Context* context, int x, int y);

private:
  enum class Presentation { Float, ActionSheet };

  void ProcessEvent(ui::Event& event) override;
  void OnDetach(ui::Element* element) override;
  std::vector<ContextMenuAction> CollectActions(const ContextMenuRequest& request) const;
  std::vector<ContextMenuAction> BuildTextActions() const;
  void RenderMenu(const ContextMenuRequest& request, const std::vector<ContextMenuAction>& actions,
                   Presentation presentation);
  void ClampFloatPanel(ui::Vector2i preferred);
  void LayoutActionSheet();
  int FindMenuItemIndex(ui::Element* target) const;
  void HandleMenuAction(int index);
  void RequestDismiss(bool restore_focus = false);

  ui::Context* context_ = nullptr;
  ui::Context* menu_context_ = nullptr;
  ui::Element* menu_target_ = nullptr;
  ui::Element* menu_editor_ = nullptr;
  /// Focused element when the menu opened; restored on outside / Escape / Cancel dismiss.
  ui::Element* focus_restore_ = nullptr;
  ui::Element* layer_ = nullptr;
  ui::Element* panel_ = nullptr;
  bool dismiss_pending_ = false;
  bool restore_focus_on_dismiss_ = false;
  bool compact_layout_ = false;
  Presentation presentation_ = Presentation::Float;
  std::string copy_snapshot_;
  std::vector<ContextMenuAction> active_actions_;
  std::vector<std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)>> providers_;
};

} // namespace pbr
