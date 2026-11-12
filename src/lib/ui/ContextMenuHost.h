#pragma once

#include <ui/dom/Event.h>
#include <ui/dom/EventListener.h>
#include <ui/base/ObserverPtr.h>
#include <ui/base/Types.h>
#include <ui/base/Vector2.h>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
} // namespace ui

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
  /// When true and the menu is anchored to an item, the action goes into the row above the item
  /// (reaction emoji over a chat message) instead of the list below it.
  bool quick = false;
  /// A quick action kept at the right end of the phone sheet's quick row, outside its sideways scroll (the reactions
  /// row's "+", like WhatsApp; Kenneth 2026-11-05 原话 93). An `expand` action is kept there too.
  bool pinned = false;
  /// A plain "Cancel" row: the phone sheet leaves it out (its close disc, the scrim and a drag down cancel there).
  bool cancel = false;
  /// Phone sheet only: a tap on this action does not run it; the sheet grows to its large stop and hands its body
  /// (`mount`, emptied of the rows) to be filled, e.g. every emoji under the reactions row's "+" (Kenneth 2026-11-04:
  /// "全部展现在这个小范围内……可以学下 whatsup"). `done` closes the sheet once something there was chosen.
  /// Elsewhere (the desktop menu) the action runs as usual.
  std::function<void(ui::Element* mount, std::function<void()> done)> expand;
  /// Optional one-line explanation under the label (pickers whose choices need a sentence).
  std::string detail;
};

struct ContextMenuRequest {
  ui::Vector2i position;
  ui::Element* target = nullptr;
  ui::Context* context = nullptr;
  /** Opened by a long press (touch) rather than a pointer. */
  bool touch = false;
};

/** Anchor a float menu just below an element (left-aligned). */
ui::Vector2i MenuPositionBelow(ui::Element* element, float gap_px = 4.f);
/** Anchor a float menu below an element, right-aligned to ~menu_min_width_px. */
ui::Vector2i MenuPositionBelowRightAligned(ui::Element* element, float menu_min_width_px = 180.f, float gap_px = 4.f);
/** Anchor a float menu that ends just above an element (left-aligned): a menu over the composer's buttons. */
ui::Vector2i MenuPositionAboveEvent(ui::Event& ev, float gap_px = 4.f);
/** Resolve current/target element from an event, then MenuPositionBelow. */
ui::Vector2i MenuPositionBelowEvent(ui::Event& ev, float gap_px = 4.f);
/** Resolve current/target element from an event, then MenuPositionBelowRightAligned. */
ui::Vector2i MenuPositionBelowRightAlignedEvent(ui::Event& ev, float menu_min_width_px = 180.f, float gap_px = 4.f);

class ContextMenuHost : public ui::EventListener {
public:
  static ContextMenuHost& Instance();

  void Install(ui::Context* context);
  /// Compact layout uses the small bottom sheet for ShowActions and for long-press item menus; floats stay clamped.
  void SetCompactLayout(bool compact);
  /// Height at the bottom of the window that is covered (on-screen keyboard, bottom chrome); menus stay above it.
  /// An open bottom sheet is placed again when the value changes (the keyboard hides while it is up).
  void SetBottomInsetDp(int inset_dp);
  /// The home-indicator strip at the bottom of a phone screen (0 while the keyboard is up): the sheet's rows stay above it.
  void SetHomeIndicatorDp(int inset_dp) { home_indicator_dp_ = inset_dp; }
  /// The title of the phone sheet a long press opens (a chat message, a chat row), "" for none.
  void SetTitleResolver(std::function<std::string(const ContextMenuRequest&)> resolver) {
    title_resolver_ = std::move(resolver);
  }
  void RegisterProvider(std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> provider);
  /// Maps a pressed element to the item it belongs to (a chat message row), or nullptr. A menu for such an
  /// item shows only the providers' actions, the list below the item and `quick` actions above it.
  /// A descendant with the attribute `menu-anchor-end` marks where the item's body ends: the list opens there.
  void SetAnchorResolver(std::function<ui::Element*(ui::Element* target)> resolver) {
    anchor_resolver_ = std::move(resolver);
  }
  void ShowAt(const ContextMenuRequest& request);
  /// Show an explicit action list (no copy/select/paste text actions). `on_dismissed` runs once if the menu goes
  /// away without any of its actions running (tap outside, Escape, another menu replacing it). On a phone it is the
  /// small sheet (DESIGN-pp-ui 8.9: the page sheets' grab handle, centred `title` and grey close disc over one card of
  /// rows); the desktop float menu shows no title.
  void ShowActions(ui::Vector2i position, std::vector<ContextMenuAction> actions,
                   std::function<void()> on_dismissed = {}, std::string title = {});
  /// ShowActions for a list that belongs above `position` (a picker over the composer): a float menu ends there
  /// when it fits above, so it does not cover what is being typed. Compact layout: the small sheet as usual.
  void ShowActionsAbove(ui::Vector2i position, std::vector<ContextMenuAction> actions,
                        std::function<void()> on_dismissed = {}, std::string title = {});
  /// ShowActions as a pull-down from a header button: a float menu under `anchor`, its right edge on the button's
  /// right edge. Compact layout too (no action sheet); there the panel gets the phone pull-down look.
  void ShowActionsPopover(ui::Element* anchor, std::vector<ContextMenuAction> actions,
                          std::function<void()> on_dismissed = {});
  /// ShowActionsPopover that opens below `anchor` (8dp under it) with its left edge on the button's left edge, a
  /// narrow 150dp card; it flips above only where there is no room below. The Home composer's plus. `above`: the card
  /// opens 8dp over the button instead (a chat's composer sits at the screen bottom).
  void ShowActionsPopoverLeftAligned(ui::Element* anchor, std::vector<ContextMenuAction> actions,
                                     std::function<void()> on_dismissed = {}, bool above = false);
  void Dismiss();
  bool IsOpen() const { return layer_ != nullptr; }
  bool HandleDismiss();
  /// Apply a deferred dismiss after the current pointer event finishes (safe DOM teardown).
  void Update();

  void OnLongPress(ui::Vector2i position, ui::Element* target);
  bool OnContextPointer(ui::Context* context, int x, int y);

private:
  /** Bar: the text menu of an input field, one horizontal row placed clear of the field. */
  enum class Presentation { Float, ActionSheet, Bar };

  void ProcessEvent(ui::Event& event) override;
  void OnDetach(ui::Element* element) override;
  std::vector<ContextMenuAction> CollectActions(const ContextMenuRequest& request) const;
  std::vector<ContextMenuAction> BuildTextActions() const;
  void RenderMenu(const ContextMenuRequest& request, const std::vector<ContextMenuAction>& actions,
                  Presentation presentation);
  void ClampFloatPanel(ui::Vector2i preferred);
  void LayoutBar(ui::Vector2i touch);
  void LayoutAnchored(ui::Vector2i touch);
  void LayoutActionSheet();
  /// The phone sheet's own pointer handling: a drag down closes it, a row runs on its click (not its press, so a
  /// drag or a scroll that starts on a row does not run it). True when the event was the sheet's.
  bool HandleSheetEvent(ui::Event& event);
  void EndSheetDrag();
  /// Keyboard inset in px still to be cleared below the layer's bottom edge (0 when the document was already lifted above the keyboard).
  float BottomInsetPx() const;
  int FindMenuItemIndex(ui::Element* target) const;
  void HandleMenuAction(int index);
  /// The phone sheet grows for its `expand` action at `index` (see ContextMenuAction::expand).
  void ExpandSheet(int index);
  /// Writes the grown sheet's height (the large stop minus the keyboard); on expanding and when the inset changes.
  void ApplyExpandedSheetHeight();
  void RequestDismiss(bool restore_focus = false);
  /// A long press opened the phone's item sheet while a text field has the focus: the finger's lift must not move the
  /// focus to the pressed item (the keyboard would close under the sheet). Undone in Dismiss().
  void HoldTypingFocus(ui::Element* target);
  /// The part both share: while a text field has the focus, the item `target` would take it from the lift is held.
  void HoldFocusFrom(ui::Element* target);
  /// A long press on an item that has no menu: the lift is no tap (no click, no focus move). Ended in Update().
  void SwallowEmptyLongPressRelease(ui::Element* target);
  void ReleaseGuardEnd();

  ui::Context* context_ = nullptr;
  ui::Context* menu_context_ = nullptr;
  ui::Element* menu_target_ = nullptr;
  ui::Element* menu_editor_ = nullptr;
  ui::Element* menu_anchor_ = nullptr;
  /// The open menu came from a long press (touch): a text field then gets the two-step phone menu.
  bool menu_touch_ = false;
  ui::Vector2i last_position_;
  /// Set by Select / Select all: once this menu is gone, open the selection menu for the same field.
  std::optional<ContextMenuRequest> reopen_request_;
  std::function<ui::Element*(ui::Element*)> anchor_resolver_;
  /// Focused element when the menu opened; restored on outside / Escape / Cancel dismiss.
  // Observer, not a raw pointer: the focused element can be destroyed (pane remount)
  // while the menu is open, and Dismiss() must not focus a freed element.
  ui::ObserverPtr<ui::Element> focus_restore_;
  /// The pressed item held from taking the focus while the sheet is open (HoldTypingFocus).
  ui::ObserverPtr<ui::Element> held_focus_taker_;
  /// SwallowEmptyLongPressRelease: the document the guard listens on, and where it stands.
  ui::ObserverPtr<ui::Element> release_guard_document_;
  bool release_guard_swallow_click_ = false;
  bool release_guard_done_ = false;
  ui::Element* layer_ = nullptr;
  ui::Element* panel_ = nullptr;
  bool dismiss_pending_ = false;
  /** A ShowActions() made while a dismiss is pending (a second step opened from a menu item); runs in Update(). */
  struct PendingShow {
    ui::Vector2i position;
    std::vector<ContextMenuAction> actions;
    bool above = false;
    bool popover = false;
    bool left = false;
    std::function<void()> on_dismissed;
    std::string title;
  };
  /// The open action list's "closed without choosing" notice; cleared once an action runs.
  std::function<void()> on_dismissed_;
  /// The open float menu ends at its position instead of starting there (ShowActionsAbove).
  bool float_above_ = false;
  bool next_show_above_ = false;
  /// The open float menu is a pull-down: its position is the top-right corner (ShowActionsPopover).
  bool float_popover_ = false;
  bool next_show_popover_ = false;
  /// The open pull-down is the left-aligned card (ShowActionsPopoverLeftAligned).
  bool float_left_ = false;
  bool next_show_left_ = false;
  std::optional<PendingShow> pending_show_;
  bool restore_focus_on_dismiss_ = false;
  bool compact_layout_ = false;
  int bottom_inset_dp_ = 0;
  int home_indicator_dp_ = 0;
  /// The open sheet's title.
  std::string title_;
  std::function<std::string(const ContextMenuRequest&)> title_resolver_;
  /// A press on the open sheet, followed to tell a tap from a drag down (or a scroll of its rows).
  bool sheet_tracking_ = false;
  bool sheet_dragging_ = false;
  int sheet_press_x_ = 0;
  int sheet_press_y_ = 0;
  ui::Element* sheet_press_target_ = nullptr;
  /// The last two finger positions (px) and their times, for the speed of a flick down.
  int sheet_last_y_ = 0;
  int sheet_prev_y_ = 0;
  std::chrono::steady_clock::time_point sheet_last_at_{};
  std::chrono::steady_clock::time_point sheet_prev_at_{};
  /// The open sheet has grown for an `expand` action: its body is that action's.
  bool sheet_expanded_ = false;
  /// The click that ends a drag or a scroll must not run the row it lands on.
  bool sheet_swallow_click_ = false;
  std::chrono::steady_clock::time_point sheet_swallow_since_{};
  Presentation presentation_ = Presentation::Float;
  std::string copy_snapshot_;
  std::vector<ContextMenuAction> active_actions_;
  std::vector<std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)>> providers_;
};

} // namespace pbr
