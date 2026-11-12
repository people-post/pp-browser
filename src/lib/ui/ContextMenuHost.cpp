#include "lib/ui/ContextMenuHost.h"

#include "common/ui/RmlEscape.h"

#include "foundation/i18n/LocalizationService.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/widgets/ElementFormControlInput.h>
#include <ui/widgets/ElementFormControlTextArea.h>
#include <ui/dom/Event.h>
#include <ui/base/Input.h>
#include <ui/dom/SelectionController.h>
#include <ui/base/SystemInterface.h>

#include <climits>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <sstream>

namespace pbr {

namespace {

ui::Element* EventAnchorElement(ui::Event& ev) {
  ui::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  return target;
}

} // namespace

ui::Vector2i MenuPositionBelow(ui::Element* element, float gap_px) {
  ui::Vector2i position{0, 0};
  if (!element) {
    return position;
  }
  const ui::Vector2f offset = element->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Box& box = element->GetBox();
  position.x = static_cast<int>(offset.x);
  position.y = static_cast<int>(offset.y + box.GetSize(ui::BoxArea::Border).y + gap_px);
  return position;
}

ui::Vector2i MenuPositionBelowRightAligned(ui::Element* element, float menu_min_width_px, float gap_px) {
  ui::Vector2i position{0, 0};
  if (!element) {
    return position;
  }
  const ui::Vector2f offset = element->GetAbsoluteOffset(ui::BoxArea::Border);
  const ui::Vector2f size = element->GetBox().GetSize(ui::BoxArea::Border);
  position.x = static_cast<int>(offset.x + size.x - menu_min_width_px);
  if (position.x < 0) {
    position.x = static_cast<int>(offset.x);
  }
  position.y = static_cast<int>(offset.y + size.y + gap_px);
  return position;
}

ui::Vector2i MenuPositionAboveEvent(ui::Event& ev, const float gap_px) {
  ui::Element* element = EventAnchorElement(ev);
  if (!element) {
    return {0, 0};
  }
  const ui::Vector2f offset = element->GetAbsoluteOffset(ui::BoxArea::Border);
  return {static_cast<int>(offset.x), static_cast<int>(offset.y - gap_px)};
}

ui::Vector2i MenuPositionBelowEvent(ui::Event& ev, float gap_px) {
  return MenuPositionBelow(EventAnchorElement(ev), gap_px);
}

ui::Vector2i MenuPositionBelowRightAlignedEvent(ui::Event& ev, float menu_min_width_px, float gap_px) {
  return MenuPositionBelowRightAligned(EventAnchorElement(ev), menu_min_width_px, gap_px);
}

namespace {

constexpr float kViewportMarginPx = 8.f;
constexpr float kBarGapDp = 8.f;
constexpr float kAnchorGapDp = 6.f;
constexpr float kPopoverEdgeDp = 8.f;
// The phone's small sheet (Kenneth 2026-11-04, ui-mockups/09-sheets-unify): its height follows the rows. Short menus
// stay around a third of the screen; a long one (the message menu with its reactions) may grow to half so its last,
// red row is not hidden below a scroll; anything longer scrolls inside.
constexpr float kSheetMaxShare = 0.5f;
/// The page sheets' large stop (about 4/5 of the screen): how tall the sheet grows for an `expand` action.
constexpr float kSheetLargeShare = 0.8f;
constexpr float kSheetBottomGapDp = 8.f;
constexpr int kSheetIconDp = 18;
constexpr int kSheetDeadzonePx = 8;
/// Let go this far down (share of the sheet's height), or flicked down, the sheet closes; else it springs back.
constexpr float kSheetDismissShare = 0.3f;
constexpr float kSheetFlickDpPerS = 700.f;
constexpr float kSheetFlickMinDp = 24.f;
constexpr auto kSheetSwallowWindow = std::chrono::milliseconds(300);

ui::Element* FindTextEditor(ui::Element* element) {
  for (ui::Element* node = element; node; node = node->GetParentNode()) {
    const ui::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input") {
      return node;
    }
  }
  return nullptr;
}

std::string GetEditorSelectedText(ui::Element* editor) {
  if (!editor) {
    return {};
  }
  int start = 0;
  int end = 0;
  ui::String selected;
  if (auto* textarea = ui_dynamic_cast<ui::ElementFormControlTextArea*>(editor)) {
    textarea->GetSelection(&start, &end, &selected);
  } else if (auto* input = ui_dynamic_cast<ui::ElementFormControlInput*>(editor)) {
    input->GetSelection(&start, &end, &selected);
  }
  return selected;
}

void SelectAllInEditor(ui::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  if (auto* textarea = ui_dynamic_cast<ui::ElementFormControlTextArea*>(editor)) {
    textarea->SetSelectionRange(0, INT_MAX);
  } else if (auto* input = ui_dynamic_cast<ui::ElementFormControlInput*>(editor)) {
    input->SetSelectionRange(0, INT_MAX);
  }
}

// Selects the word at the caret: the same expansion a double click does.
void SelectWordInEditor(ui::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  editor->DispatchEvent(ui::EventId::Dblclick, ui::Dictionary());
}

void DeleteEditorSelection(ui::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  if (ui::Context* context = editor->GetContext()) {
    context->ProcessKeyDown(ui::Input::KI_BACK, 0);
    context->ProcessKeyUp(ui::Input::KI_BACK, 0);
  }
}

void PasteIntoEditor(ui::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  ui::String clipboard;
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->GetClipboardText(clipboard);
  }
  if (clipboard.empty()) {
    return;
  }
  ui::Dictionary parameters;
  parameters["text"] = clipboard;
  editor->DispatchEvent(ui::EventId::Textinput, parameters);
}

enum class ActionFilter { All, Quick, List };

// The label, with its explanation under it when the action has one. Both are text people may have typed.
void AppendLabel(std::ostringstream& out, const ContextMenuAction& action) {
  if (action.detail.empty()) {
    out << "<span class=\"context-menu-item-label\">" << EscapeRml(action.label) << "</span>";
    return;
  }
  // Block elements: as spans the label and the explanation were drawn over each other (B180).
  out << "<div class=\"context-menu-item-text\"><div class=\"context-menu-item-label\">" << EscapeRml(action.label)
      << "</div><div class=\"context-menu-item-detail\">" << EscapeRml(action.detail) << "</div></div>";
}

void AppendActionButtons(std::ostringstream& out, const std::vector<ContextMenuAction>& actions,
                         const ActionFilter filter = ActionFilter::All, const int icon_dp = 16) {
  for (size_t i = 0; i < actions.size(); ++i) {
    const ContextMenuAction& action = actions[i];
    const bool enabled = !action.enabled || action.enabled();
    if (!enabled || (filter == ActionFilter::Quick && !action.quick) ||
        (filter == ActionFilter::List && action.quick)) {
      continue;
    }
    out << "<button class=\"context-menu-item";
    if (action.danger) {
      out << " context-menu-item--danger";
    }
    if (action.selected) {
      out << " context-menu-item--selected";
    }
    out << "\" type=\"button\" data-item-index=\"" << i << "\">";
    if (!action.icon.empty()) {
      out << "<div class=\"context-menu-item-icon\"><svg src=\"" << action.icon << "\" width=\"" << icon_dp
          << "\" height=\"" << icon_dp << "\" crop-to-content=\"true\"></svg></div>";
    }
    // Labels carry names people typed (share targets, group members).
    AppendLabel(out, action);
    if (action.selected) {
      out << "<span class=\"context-menu-item-check\">✓</span>";
    }
    out << "</button>";
  }
}

// The phone sheet's rows, like iOS's own context menu (Kenneth 2026-11-04: the leading 22dp icons looked big, the
// hairlines deliberate): one card of 50dp rows, the label on the left, a small 18dp icon on the right in the secondary
// label colour, a very faint hairline inset 16dp on both sides between rows, red text and icon for a destructive row.
// Quick actions (reactions) are a row of their own above the card. A plain Cancel row is left out: the close disc
// stands for it.
// `expanders`: of the quick actions, only the pinned ones or those that grow the sheet (the reactions row's "+", kept
// at its right end outside the row's sideways scroll), or only the others.
void AppendSheetRows(std::ostringstream& out, const std::vector<ContextMenuAction>& actions, const bool quick,
                     const bool expanders = false) {
  for (size_t i = 0; i < actions.size(); ++i) {
    const ContextMenuAction& action = actions[i];
    const bool enabled = !action.enabled || action.enabled();
    if (!enabled || action.quick != quick || (!quick && action.cancel) ||
        (quick && (action.pinned || static_cast<bool>(action.expand)) != expanders)) {
      continue;
    }
    out << "<button class=\"context-menu-item";
    if (action.danger) {
      out << " context-menu-item--danger";
    }
    if (action.selected) {
      out << " context-menu-item--selected";
    }
    if (!quick && !action.icon.empty()) {
      out << " context-menu-item--icon";
    }
    out << "\" type=\"button\" data-item-index=\"" << i << "\">";
    if (quick) {
      out << "<span class=\"context-menu-item-label\">" << EscapeRml(action.label) << "</span></button>";
      continue;
    }
    out << "<span class=\"context-menu-item-body\">";
    AppendLabel(out, action);
    if (action.selected) {
      out << "<span class=\"context-menu-item-check\">✓</span>";
    }
    if (!action.icon.empty()) {
      out << "<div class=\"context-menu-item-icon\"><svg src=\"" << action.icon << "\" width=\"" << kSheetIconDp
          << "\" height=\"" << kSheetIconDp << "\" crop-to-content=\"true\"></svg></div>";
    }
    out << "</span></button>";
  }
}

double ToSeconds(const std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double>(d).count();
}

} // namespace

ContextMenuHost& ContextMenuHost::Instance() {
  static ContextMenuHost host;
  return host;
}

void ContextMenuHost::Install(ui::Context* context) {
  context_ = context;
  if (!context_) {
    return;
  }
  // A long press opens this host's menu for the pressed item; it does not start a word selection with handles.
  context_->SetTouchLongPressSelectsText(false);
  context_->SetTouchLongPressCallback(
      [](ui::Vector2i position, ui::Element* target) { ContextMenuHost::Instance().OnLongPress(position, target); });
}

void ContextMenuHost::SetCompactLayout(bool compact) {
  if (compact_layout_ == compact) {
    return;
  }
  compact_layout_ = compact;
  if (IsOpen() || dismiss_pending_) {
    Dismiss();
  }
}

void ContextMenuHost::RegisterProvider(
    std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> provider) {
  providers_.push_back(std::move(provider));
}

void ContextMenuHost::OnLongPress(ui::Vector2i position, ui::Element* target) {
  if (!context_) {
    return;
  }
  ContextMenuRequest request;
  request.position = position;
  request.target = target;
  request.context = context_;
  request.touch = true;
  ShowAt(request);
  HoldTypingFocus(target);
  if (!IsOpen() && target && anchor_resolver_ && anchor_resolver_(target)) {
    SwallowEmptyLongPressRelease(target);
  }
}

void ContextMenuHost::SwallowEmptyLongPressRelease(ui::Element* target) {
  // An item with no menu (a call record row): the finger's lift after the long press is no tap. Its click must not
  // reach the row (it would call back), and the lift must not move the focus off a field being typed in (the
  // keyboard would drop).
  ReleaseGuardEnd();
  ui::ElementDocument* document = target->GetOwnerDocument();
  if (!document) {
    return;
  }
  release_guard_document_ = document->GetObserverPtr();
  document->AddEventListener(ui::EventId::Mousedown, this, true);
  document->AddEventListener(ui::EventId::Mouseup, this, true);
  document->AddEventListener(ui::EventId::Click, this, true);
  release_guard_swallow_click_ = true;
  HoldFocusFrom(target);
}

void ContextMenuHost::ReleaseGuardEnd() {
  release_guard_swallow_click_ = false;
  release_guard_done_ = false;
  if (ui::Element* taker = held_focus_taker_.get()) {
    taker->RemoveProperty("focus");
  }
  held_focus_taker_ = nullptr;
  if (ui::Element* document = release_guard_document_.get()) {
    document->RemoveEventListener(ui::EventId::Mousedown, this, true);
    document->RemoveEventListener(ui::EventId::Mouseup, this, true);
    document->RemoveEventListener(ui::EventId::Click, this, true);
  }
  release_guard_document_ = nullptr;
}

void ContextMenuHost::HoldTypingFocus(ui::Element* target) {
  // Only the phone's item sheet over a field being typed in: the desktop's menus and the keyboard-less case are as before.
  if (!IsOpen() || presentation_ != Presentation::ActionSheet || !menu_anchor_) {
    return;
  }
  HoldFocusFrom(target);
}

void ContextMenuHost::HoldFocusFrom(ui::Element* target) {
  if (!context_) {
    return;
  }
  ui::Element* focus = context_->GetFocusElement();
  if (!FindTextEditor(focus)) {
    return;
  }
  // The press on the item held the focus back from the field until the finger lifts (a touch that turns into a
  // scroll keeps the keyboard), and the lift still moves it after a long press: the field lost the focus, the
  // keyboard closed under the open sheet and the sheet fell with it (iPhone, 2026-11-12). The element that would take
  // it is the one Context focuses on a press: the nearest from the target up that takes focus. It takes none while
  // the sheet is open; the field keeps typing, the keyboard stays where it is.
  ui::Element* taker = target;
  while (taker && taker->GetComputedValues().focus() == ui::Style::Focus::None) {
    taker = taker->GetParentNode();
  }
  if (!taker || taker == focus) {
    return;
  }
  taker->SetProperty("focus", "none");
  held_focus_taker_ = taker->GetObserverPtr();
}

bool ContextMenuHost::OnContextPointer(ui::Context* context, int x, int y) {
  if (!context) {
    return false;
  }
  const ui::Vector2f point(static_cast<float>(x), static_cast<float>(y));
  ui::Element* target = context->GetElementAtPoint(point);
  if (!target) {
    return false;
  }
  ContextMenuRequest request;
  request.position = {x, y};
  request.target = target;
  request.context = context;
  ShowAt(request);
  return true;
}

std::vector<ContextMenuAction> ContextMenuHost::BuildTextActions() const {
  std::vector<ContextMenuAction> actions;
  ui::Context* context = menu_context_ ? menu_context_ : context_;
  if (!context) {
    return actions;
  }

  ui::Element* editor = menu_editor_;
  ui::Element* target = menu_target_;
  ui::SelectionController* selection = context->GetSelectionController();
  const std::string snapshot = copy_snapshot_;

  auto copy_enabled = [snapshot]() { return !snapshot.empty(); };
  auto copy_run = [snapshot]() {
    if (snapshot.empty()) {
      return;
    }
    if (ui::SystemInterface* system = ui::GetSystemInterface()) {
      system->SetClipboardText(snapshot);
    }
  };

  auto select_all_enabled = [editor, selection, target]() {
    if (editor) {
      return true;
    }
    return selection && target && selection->CanSelectStaticText(target);
  };
  auto select_all_run = [editor, selection]() {
    if (editor) {
      SelectAllInEditor(editor);
      return;
    }
    if (selection) {
      selection->SelectAll();
    }
  };

  auto paste_enabled = [editor]() { return editor != nullptr; };
  auto paste_run = [editor]() { PasteIntoEditor(editor); };

  if (menu_touch_ && editor) {
    // Phone text field, two steps: with nothing selected offer Paste / Select / Select all; choosing a
    // selection reopens the menu as Copy / Cut / Paste.
    if (snapshot.empty()) {
      const ContextMenuRequest again{last_position_, target, context, true};
      actions.push_back({"paste", Tr("common.paste"), nullptr, paste_run});
      actions.push_back({"select", Tr("common.select"), nullptr, [editor, again]() {
                           SelectWordInEditor(editor);
                           Instance().reopen_request_ = again;
                         }});
      actions.push_back({"select_all", Tr("common.select_all"), nullptr, [editor, again]() {
                           SelectAllInEditor(editor);
                           Instance().reopen_request_ = again;
                         }});
      return actions;
    }
    actions.push_back({"copy", Tr("common.copy"), nullptr, copy_run});
    actions.push_back({"cut", Tr("common.cut"), nullptr, [copy_run, editor]() {
                         copy_run();
                         DeleteEditorSelection(editor);
                       }});
    actions.push_back({"paste", Tr("common.paste"), nullptr, paste_run});
    return actions;
  }

  actions.push_back({"copy", Tr("common.copy"), copy_enabled, copy_run});
  if (editor) {
    actions.push_back({"cut", Tr("common.cut"), copy_enabled, [copy_run, editor]() {
                         copy_run();
                         DeleteEditorSelection(editor);
                       }});
  }
  actions.push_back({"paste", Tr("common.paste"), paste_enabled, paste_run});
  actions.push_back({"select_all", Tr("common.select_all"), select_all_enabled, select_all_run});
  return actions;
}

std::vector<ContextMenuAction> ContextMenuHost::CollectActions(const ContextMenuRequest& request) const {
  // An item menu (chat message) is the providers' alone: no text-editing actions.
  std::vector<ContextMenuAction> actions = menu_anchor_ ? std::vector<ContextMenuAction>{} : BuildTextActions();
  for (const auto& provider : providers_) {
    if (!provider) {
      continue;
    }
    const auto provided = provider(request);
    actions.insert(actions.end(), provided.begin(), provided.end());
  }
  return actions;
}

void ContextMenuHost::SetBottomInsetDp(const int inset_dp) {
  if (inset_dp == bottom_inset_dp_) {
    return;
  }
  bottom_inset_dp_ = inset_dp;
  // The sheet's `bottom` is a pixel value written at open time: write it again, or it hangs where the keyboard was.
  if (layer_ && presentation_ == Presentation::ActionSheet) {
    LayoutActionSheet();
    if (sheet_expanded_) {
      ApplyExpandedSheetHeight(); // the grown sheet's pixel height also took the keyboard off once
    }
  }
}

float ContextMenuHost::BottomInsetPx() const {
  if (!context_) {
    return 0.f;
  }
  const float dp = context_->GetDensityIndependentPixelRatio();
  float inset = static_cast<float>(bottom_inset_dp_) * dp;
  // The on-screen keyboard also insets the document itself (ShellHost lifts its bottom edge). A fixed
  // layer then already ends above the keyboard: counting the inset again pushed the attach sheet to
  // the top of a phone screen (B90, 2026-10-23). Only the part below the layer's edge is still owed.
  if (layer_ && inset > 0.f) {
    const ui::Vector2i dims = context_->GetDimensions();
    const float layer_bottom =
        layer_->GetAbsoluteOffset(ui::BoxArea::Border).y + layer_->GetBox().GetSize(ui::BoxArea::Border).y;
    const float already = static_cast<float>(dims.y) - layer_bottom;
    if (already > 0.f) {
      inset = std::max(0.f, inset - already);
    }
  }
  return inset;
}

void ContextMenuHost::ClampFloatPanel(ui::Vector2i preferred) {
  if (!panel_ || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }

  ui::ElementDocument* document = context_->GetDocument(0);
  document->UpdateDocument();

  ui::Vector2i dims = context_->GetDimensions();
  // Positioned by `top` from the layer's top edge: the keyboard's top is at dims.y - inset whatever the
  // document's lift (only the bottom-anchored sheet measures against the layer's bottom edge).
  dims.y -= static_cast<int>(static_cast<float>(bottom_inset_dp_) * context_->GetDensityIndependentPixelRatio());
  const ui::Vector2f size = panel_->GetBox().GetSize(ui::BoxArea::Border);
  if (size.x <= 0.f || size.y <= 0.f || dims.x <= 0 || dims.y <= 0) {
    return;
  }

  float left = float_popover_ ? static_cast<float>(preferred.x) - size.x : static_cast<float>(preferred.x);
  float top = static_cast<float>(preferred.y);
  // A pull-down keeps clear of the screen edge (its corner and shadow show) even when its button sits at the edge.
  const float side_margin =
      float_popover_ ? kPopoverEdgeDp * context_->GetDensityIndependentPixelRatio() : kViewportMarginPx;
  const float max_left = static_cast<float>(dims.x) - size.x - side_margin;
  const float max_top = static_cast<float>(dims.y) - size.y - kViewportMarginPx;

  if (left > max_left) {
    left = max_left;
  }
  if (left < kViewportMarginPx) {
    left = kViewportMarginPx;
  }

  if (float_above_ && static_cast<float>(preferred.y) - size.y >= kViewportMarginPx) {
    top = static_cast<float>(preferred.y) - size.y;
  } else if (top + size.y > static_cast<float>(dims.y) - kViewportMarginPx) {
    // Prefer flipping above the preferred anchor when there is room.
    const float flipped = static_cast<float>(preferred.y) - size.y;
    if (flipped >= kViewportMarginPx) {
      top = flipped;
    } else if (max_top >= kViewportMarginPx) {
      top = max_top;
    } else {
      top = kViewportMarginPx;
    }
  }
  if (top > max_top && max_top >= kViewportMarginPx) {
    top = max_top;
  }
  if (top < kViewportMarginPx) {
    top = kViewportMarginPx;
  }

  char left_buf[32];
  char top_buf[32];
  std::snprintf(left_buf, sizeof(left_buf), "%.0fpx", left);
  std::snprintf(top_buf, sizeof(top_buf), "%.0fpx", top);
  panel_->SetProperty("left", left_buf);
  panel_->SetProperty("top", top_buf);
}

void ContextMenuHost::LayoutBar(ui::Vector2i touch) {
  if (!panel_ || !context_ || !menu_editor_ || context_->GetNumDocuments() == 0) {
    return;
  }
  context_->GetDocument(0)->UpdateDocument();

  const ui::Vector2i dims = context_->GetDimensions();
  const ui::Vector2f size = panel_->GetBox().GetSize(ui::BoxArea::Border);
  if (size.x <= 0.f || size.y <= 0.f || dims.x <= 0 || dims.y <= 0) {
    return;
  }

  // Above the field, so the row never covers the text being selected; below it only when there is no room.
  const float gap = kBarGapDp * context_->GetDensityIndependentPixelRatio();
  const float field_top = menu_editor_->GetAbsoluteOffset(ui::BoxArea::Border).y;
  const float field_bottom = field_top + menu_editor_->GetBox().GetSize(ui::BoxArea::Border).y;
  float top = field_top - gap - size.y;
  if (top < kViewportMarginPx) {
    top = field_bottom + gap;
  }
  const float max_left = static_cast<float>(dims.x) - size.x - kViewportMarginPx;
  const float left = std::max(kViewportMarginPx, std::min(static_cast<float>(touch.x) - size.x * 0.5f, max_left));

  char left_buf[32];
  char top_buf[32];
  std::snprintf(left_buf, sizeof(left_buf), "%.0fpx", left);
  std::snprintf(top_buf, sizeof(top_buf), "%.0fpx", top);
  panel_->SetProperty("left", left_buf);
  panel_->SetProperty("top", top_buf);
}

void ContextMenuHost::LayoutAnchored(ui::Vector2i touch) {
  if (!panel_ || !layer_ || !context_ || !menu_anchor_ || context_->GetNumDocuments() == 0) {
    return;
  }
  context_->GetDocument(0)->UpdateDocument();

  const float dp = context_->GetDensityIndependentPixelRatio();
  const ui::Vector2i dims = context_->GetDimensions();
  const float view_bottom = static_cast<float>(dims.y) - static_cast<float>(bottom_inset_dp_) * dp - kViewportMarginPx;
  const float gap = kAnchorGapDp * dp;
  const float anchor_top = menu_anchor_->GetAbsoluteOffset(ui::BoxArea::Border).y;
  float anchor_bottom = anchor_top + menu_anchor_->GetBox().GetSize(ui::BoxArea::Border).y;
  // The item can mark where its body ends (a chat message's reaction chips come after it); the list opens there.
  ui::ElementList body_end;
  menu_anchor_->QuerySelectorAll(body_end, "[menu-anchor-end]");
  if (!body_end.empty()) {
    anchor_bottom = body_end.front()->GetAbsoluteOffset(ui::BoxArea::Border).y;
  }

  const auto place = [&](ui::Element* element, const float top) {
    const float width = element->GetBox().GetSize(ui::BoxArea::Border).x;
    const float max_left = static_cast<float>(dims.x) - width - kViewportMarginPx;
    const float left = std::max(kViewportMarginPx, std::min(static_cast<float>(touch.x) - width * 0.5f, max_left));
    char left_buf[32];
    char top_buf[32];
    std::snprintf(left_buf, sizeof(left_buf), "%.0fpx", left);
    std::snprintf(top_buf, sizeof(top_buf), "%.0fpx", top);
    element->SetProperty("left", left_buf);
    element->SetProperty("top", top_buf);
  };

  // Quick row above the item, list below it; a message taller than the view keeps both on screen.
  float list_min_top = kViewportMarginPx;
  if (ui::Element* quick = layer_->GetElementById("context-menu-quick")) {
    const float quick_height = quick->GetBox().GetSize(ui::BoxArea::Border).y;
    const float quick_top = std::max(kViewportMarginPx, anchor_top - gap - quick_height);
    place(quick, quick_top);
    list_min_top = quick_top + quick_height + gap;
  }
  const float list_height = panel_->GetBox().GetSize(ui::BoxArea::Border).y;
  const float list_top = std::max(list_min_top, std::min(anchor_bottom + gap, view_bottom - list_height));
  place(panel_, list_top);
}

void ContextMenuHost::LayoutActionSheet() {
  if (!panel_ || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  // The layer was just appended: lay it out so BottomInsetPx() measures a real bottom edge.
  context_->GetDocument(0)->UpdateDocument();

  const ui::Vector2i dims = context_->GetDimensions();
  if (dims.x <= 0 || dims.y <= 0) {
    return;
  }

  // Edge to edge at the bottom, above the keyboard when it is up; the rows clear the home indicator.
  const float dp = context_->GetDensityIndependentPixelRatio();
  const float bottom = BottomInsetPx();
  const float pad_bottom = (kSheetBottomGapDp + static_cast<float>(bottom_inset_dp_ > 0 ? 0 : home_indicator_dp_)) * dp;
  char bottom_buf[32];
  char pad_buf[32];
  std::snprintf(bottom_buf, sizeof(bottom_buf), "%.0fpx", bottom);
  std::snprintf(pad_buf, sizeof(pad_buf), "%.0fpx", pad_bottom);
  panel_->SetProperty("left", "0px");
  panel_->SetProperty("right", "0px");
  panel_->SetProperty("bottom", bottom_buf);
  panel_->SetProperty("padding-bottom", pad_buf);

  // Up to about a third of the screen above the home indicator: the card of rows scrolls when there are more.
  ui::Element* list = layer_ ? layer_->GetElementById("context-menu-sheet-list") : nullptr;
  if (!list) {
    return;
  }
  context_->GetDocument(0)->UpdateDocument();
  const float list_top =
      list->GetAbsoluteOffset(ui::BoxArea::Border).y - panel_->GetAbsoluteOffset(ui::BoxArea::Border).y;
  const float max_list =
      std::max(2.f * 50.f * dp, static_cast<float>(dims.y) * kSheetMaxShare - list_top - kSheetBottomGapDp * dp);
  char max_buf[32];
  std::snprintf(max_buf, sizeof(max_buf), "%.0fpx", max_list);
  list->SetProperty("max-height", max_buf);
}

void ContextMenuHost::RenderMenu(const ContextMenuRequest& request, const std::vector<ContextMenuAction>& actions,
                                 Presentation presentation) {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* document = context_->GetDocument(0);
  ui::Element* body = document;
  if (ui::Element* shell_root = document->GetElementById("shell-root")) {
    if (ui::Element* shell_body = shell_root->GetParentNode()) {
      body = shell_body;
    }
  }
  if (!body) {
    return;
  }

  presentation_ = presentation;

  // Outer layer is created via CreateElement; InnerRML must only contain its children
  // (scrim + panel). Nesting another unclosed context-menu-layer div caused XML parse
  // errors and crashes on dismiss/outside click.
  std::ostringstream out;
  out << "<div class=\"context-menu-scrim\" id=\"context-menu-scrim\"></div>";
  if (presentation == Presentation::ActionSheet) {
    // The page sheets' family (Kenneth 2026-11-04): grab handle, centred title, grey close disc top right.
    out << "<div class=\"context-menu-sheet\" id=\"context-menu-panel\">";
    out << "<div class=\"context-menu-sheet-grabber\"></div>";
    out << "<div class=\"context-menu-sheet-header\">";
    if (!title_.empty()) {
      out << "<p class=\"context-menu-sheet-title\" id=\"context-menu-sheet-title\">" << EscapeRml(title_) << "</p>";
    }
    out << "</div>";
    out << "<button class=\"context-menu-sheet-close\" type=\"button\" id=\"context-menu-cancel\" title=\""
        << EscapeRml(Tr("common.close")) << "\"><span class=\"context-menu-sheet-disc\">"
        << "<svg src=\"../icons/v2/close.svg\" width=\"12\" height=\"12\"></svg></span></button>";
    if (std::any_of(actions.begin(), actions.end(),
                    [](const ContextMenuAction& a) { return a.quick && (!a.enabled || a.enabled()); })) {
      // The reactions scroll sideways when there are more than fit; the "+" stays at the right end.
      out << "<div class=\"context-menu-sheet-quick\"><div class=\"context-menu-sheet-quick-scroll\">";
      AppendSheetRows(out, actions, /*quick=*/true);
      out << "</div>";
      AppendSheetRows(out, actions, /*quick=*/true, /*expanders=*/true);
      out << "</div>";
    }
    out << "<div class=\"context-menu-sheet-list\" id=\"context-menu-sheet-list\">";
    AppendSheetRows(out, actions, /*quick=*/false);
    out << "</div>";
    out << "<div class=\"context-menu-sheet-expand\" id=\"context-menu-sheet-expand\"></div>";
    out << "</div>";
  } else {
    const bool anchored = presentation == Presentation::Float && menu_anchor_;
    const bool phone_popover = float_popover_ && compact_layout_;
    const bool has_quick =
        anchored && std::any_of(actions.begin(), actions.end(), [](const ContextMenuAction& a) { return a.quick; });
    if (has_quick) {
      out << "<div class=\"context-menu-panel context-menu-quick\" id=\"context-menu-quick\">";
      AppendActionButtons(out, actions, ActionFilter::Quick);
      out << "</div>";
    }
    out << "<div class=\"context-menu-panel\" id=\"context-menu-panel\" style=\"left: " << request.position.x
        << "px; top: " << request.position.y << "px;\">";
    AppendActionButtons(out, actions, anchored ? ActionFilter::List : ActionFilter::All, phone_popover ? 20 : 16);
    out << "</div>";
  }

  ui::ElementPtr layer_element = document->CreateElement("div");
  layer_element->SetAttribute("id", "context-menu-layer");
  layer_element->SetClass("context-menu-layer", true);
  if (presentation == Presentation::ActionSheet) {
    layer_element->SetClass("context-menu-layer--sheet", true);
  }
  if (presentation == Presentation::Bar) {
    layer_element->SetClass("context-menu-layer--bar", true);
  }
  if (presentation == Presentation::Float && float_popover_ && compact_layout_) {
    layer_element->SetClass("context-menu-layer--popover", true);
  }
  if (presentation == Presentation::Float && float_popover_ && float_left_ && compact_layout_) {
    layer_element->SetClass("context-menu-layer--popover-left", true);
  }
  layer_element->SetInnerRML(out.str());
  layer_ = body->AppendChild(std::move(layer_element));
  panel_ = layer_ ? layer_->GetElementById("context-menu-panel") : nullptr;
  if (!layer_) {
    return;
  }
  layer_->AddEventListener(ui::EventId::Mousedown, this, true);
  layer_->AddEventListener(ui::EventId::Click, this, true);
  if (presentation == Presentation::ActionSheet) {
    layer_->AddEventListener(ui::EventId::Mousemove, this, true);
    layer_->AddEventListener(ui::EventId::Mouseup, this, true);
  }

  if (presentation == Presentation::Float && menu_anchor_) {
    LayoutAnchored(request.position);
  } else if (presentation == Presentation::Float) {
    ClampFloatPanel(request.position);
  } else if (presentation == Presentation::Bar) {
    LayoutBar(request.position);
  } else {
    LayoutActionSheet();
  }
}

void ContextMenuHost::ShowAt(const ContextMenuRequest& request) {
  Dismiss();
  menu_touch_ = request.touch;
  last_position_ = request.position;
  menu_context_ = request.context ? request.context : context_;
  menu_target_ = request.target;
  menu_editor_ = nullptr;
  menu_anchor_ = nullptr;
  focus_restore_ = nullptr;
  copy_snapshot_.clear();
  if (menu_context_) {
    ui::Element* focus = menu_context_->GetFocusElement();
    menu_editor_ = FindTextEditor(focus ? focus : menu_target_);
    if (ui::Element* restore = focus ? focus : menu_editor_) {
      focus_restore_ = restore->GetObserverPtr();
    }
    menu_anchor_ = anchor_resolver_ ? anchor_resolver_(menu_target_) : nullptr;
    if (menu_anchor_) {
      // The press was on an item, whatever field happens to hold the focus.
      menu_editor_ = nullptr;
    }
    if (menu_editor_) {
      copy_snapshot_ = GetEditorSelectedText(menu_editor_);
    } else if (ui::SelectionController* selection = menu_context_->GetSelectionController()) {
      copy_snapshot_ = selection->GetSelectedText();
    }
  }

  active_actions_ = CollectActions(request);
  bool any_enabled = false;
  for (const ContextMenuAction& action : active_actions_) {
    if (!action.enabled || action.enabled()) {
      any_enabled = true;
      break;
    }
  }
  if (!any_enabled) {
    active_actions_.clear();
    menu_context_ = nullptr;
    menu_target_ = nullptr;
    menu_editor_ = nullptr;
    menu_anchor_ = nullptr;
    focus_restore_ = nullptr;
    return;
  }
  // A text field's menu is a row above the field, for a pointer as for a finger. On a phone any other long-press
  // menu (a chat message, a chat row) is the small sheet like every other phone menu (Kenneth 2026-11-04); the
  // desktop's stays anchored near the pointer.
  const Presentation presentation = menu_editor_      ? Presentation::Bar
                                    : compact_layout_ ? Presentation::ActionSheet
                                                      : Presentation::Float;
  title_ = presentation == Presentation::ActionSheet && title_resolver_ ? title_resolver_(request) : std::string();
  RenderMenu(request, active_actions_, presentation);
}

void ContextMenuHost::ShowActionsAbove(ui::Vector2i position, std::vector<ContextMenuAction> actions,
                                       std::function<void()> on_dismissed, std::string title) {
  next_show_above_ = true;
  ShowActions(position, std::move(actions), std::move(on_dismissed), std::move(title));
}

void ContextMenuHost::ShowActionsPopover(ui::Element* anchor, std::vector<ContextMenuAction> actions,
                                         std::function<void()> on_dismissed) {
  ui::Vector2i position{0, 0};
  if (anchor) {
    const ui::Vector2f offset = anchor->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = anchor->GetBox().GetSize(ui::BoxArea::Border);
    const float gap = context_ ? kAnchorGapDp * context_->GetDensityIndependentPixelRatio() : 4.f;
    position.x = static_cast<int>(offset.x + size.x);
    position.y = static_cast<int>(offset.y + size.y + gap);
  }
  next_show_popover_ = true;
  ShowActions(position, std::move(actions), std::move(on_dismissed));
}

void ContextMenuHost::ShowActionsPopoverLeftAligned(ui::Element* anchor, std::vector<ContextMenuAction> actions,
                                                    std::function<void()> on_dismissed, const bool above) {
  ui::Vector2i position{0, 0};
  if (anchor) {
    const ui::Vector2f offset = anchor->GetAbsoluteOffset(ui::BoxArea::Border);
    const ui::Vector2f size = anchor->GetBox().GetSize(ui::BoxArea::Border);
    const float ratio = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
    // A pull-down is laid out by its right edge; the card is 150dp wide (components.rcss, --popover-left), so that edge
    // goes 150dp right of the button's left, and the card starts 8dp under the button.
    position.x = static_cast<int>(offset.x + 150.f * ratio);
    position.y = static_cast<int>(above ? offset.y - 8.f * ratio : offset.y + size.y + 8.f * ratio);
  }
  next_show_above_ = above;
  next_show_left_ = true;
  next_show_popover_ = true;
  ShowActions(position, std::move(actions), std::move(on_dismissed));
}

void ContextMenuHost::ShowActions(ui::Vector2i position, std::vector<ContextMenuAction> actions,
                                  std::function<void()> on_dismissed, std::string title) {
  const bool above = std::exchange(next_show_above_, false);
  const bool popover = std::exchange(next_show_popover_, false);
  const bool left = std::exchange(next_show_left_, false);
  if (dismiss_pending_) {
    // Opened from a menu item's click (Mute…, React): the first menu's DOM is still dispatching that
    // event, which is why its teardown was deferred. Tearing it down here instead would leave the
    // dangling Context::active pointers RequestDismiss() exists to avoid. Open this one from Update().
    pending_show_ =
        PendingShow{position, std::move(actions), above, popover, left, std::move(on_dismissed), std::move(title)};
    return;
  }
  Dismiss(); // a menu still open went away without a choice: its notice runs in there
  float_above_ = above;
  float_popover_ = popover;
  float_left_ = left;
  if (!context_ || actions.empty()) {
    if (on_dismissed) {
      on_dismissed(); // nothing was shown, so nothing can be chosen
    }
    return;
  }
  on_dismissed_ = std::move(on_dismissed);

  menu_context_ = context_;
  menu_target_ = nullptr;
  menu_editor_ = nullptr;
  menu_anchor_ = nullptr;
  focus_restore_ = nullptr;
  if (ui::Element* focus = menu_context_ ? menu_context_->GetFocusElement() : nullptr) {
    focus_restore_ = focus->GetObserverPtr();
  }
  copy_snapshot_.clear();
  active_actions_ = std::move(actions);

  bool any_enabled = false;
  for (const ContextMenuAction& action : active_actions_) {
    if (!action.enabled || action.enabled()) {
      any_enabled = true;
      break;
    }
  }
  if (!any_enabled) {
    active_actions_.clear();
    menu_context_ = nullptr;
    focus_restore_ = nullptr;
    return;
  }

  ContextMenuRequest request;
  request.position = position;
  request.context = context_;
  const Presentation presentation =
      compact_layout_ && !float_popover_ ? Presentation::ActionSheet : Presentation::Float;
  title_ = presentation == Presentation::ActionSheet ? std::move(title) : std::string();
  RenderMenu(request, active_actions_, presentation);
}

void ContextMenuHost::Dismiss() {
  dismiss_pending_ = false;
  const std::function<void()> dismissed = std::exchange(on_dismissed_, nullptr);
  const bool restore_focus = restore_focus_on_dismiss_;
  restore_focus_on_dismiss_ = false;
  ui::ObserverPtr<ui::Element> restore = restore_focus ? std::move(focus_restore_) : ui::ObserverPtr<ui::Element>();
  focus_restore_ = nullptr;
  // Capture and clear before RemoveEventListener: DetachEvent invokes OnDetach, which
  // would otherwise null layer_ mid-function and crash the second RemoveEventListener.
  ui::Element* layer = layer_;
  EndSheetDrag();
  sheet_swallow_click_ = false;
  sheet_press_target_ = nullptr;
  sheet_expanded_ = false;
  title_.clear();
  layer_ = nullptr;
  panel_ = nullptr;
  presentation_ = Presentation::Float;
  float_above_ = false;
  float_popover_ = false;
  float_left_ = false;
  if (layer) {
    layer->RemoveEventListener(ui::EventId::Mousedown, this, true);
    layer->RemoveEventListener(ui::EventId::Click, this, true);
    layer->RemoveEventListener(ui::EventId::Mousemove, this, true);
    layer->RemoveEventListener(ui::EventId::Mouseup, this, true);
    if (ui::Element* parent = layer->GetParentNode()) {
      parent->RemoveChild(layer);
    }
  }
  active_actions_.clear();
  menu_context_ = nullptr;
  menu_target_ = nullptr;
  menu_editor_ = nullptr;
  menu_anchor_ = nullptr;
  copy_snapshot_.clear();
  if (ui::Element* taker = held_focus_taker_.get()) {
    taker->RemoveProperty("focus"); // a tap on the item puts the keyboard away again
  }
  held_focus_taker_ = nullptr;
  // Resolved only now: removing the layer above can destroy it, and so can a
  // remount that happened while the menu was open.
  if (ui::Element* element = restore.get()) {
    element->Focus();
  }
  if (dismissed) {
    dismissed();
  }
}

void ContextMenuHost::RequestDismiss(bool restore_focus) {
  // Defer DOM removal until after the current pointer event finishes. Removing the
  // hover/active target mid-mousedown leaves dangling Context::active pointers.
  if (restore_focus) {
    restore_focus_on_dismiss_ = true;
  }
  dismiss_pending_ = true;
}

void ContextMenuHost::Update() {
  if (release_guard_done_) {
    ReleaseGuardEnd();
  }
  if (dismiss_pending_) {
    Dismiss();
  }
  if (pending_show_) {
    PendingShow show = std::move(*pending_show_);
    pending_show_.reset();
    next_show_above_ = show.above;
    next_show_popover_ = show.popover;
    next_show_left_ = show.left;
    ShowActions(show.position, std::move(show.actions), std::move(show.on_dismissed), std::move(show.title));
  }
  // Not from inside the menu's own click: the old layer had to be gone first.
  if (const auto again = std::exchange(reopen_request_, std::nullopt)) {
    ShowAt(*again);
  }
}

bool ContextMenuHost::HandleDismiss() {
  if (!IsOpen() && !dismiss_pending_) {
    return false;
  }
  restore_focus_on_dismiss_ = true;
  Dismiss();
  return true;
}

void ContextMenuHost::OnDetach(ui::Element* element) {
  if (element == layer_) {
    layer_ = nullptr;
    panel_ = nullptr;
  } else if (element == panel_) {
    panel_ = nullptr;
  }
}

int ContextMenuHost::FindMenuItemIndex(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    const ui::Variant* index_variant = node->GetAttribute("data-item-index");
    if (!index_variant) {
      continue;
    }
    const int index = index_variant->Get<int>(-1);
    if (index >= 0) {
      return index;
    }
  }
  return -1;
}

void ContextMenuHost::HandleMenuAction(int index) {
  if (index < 0 || index >= static_cast<int>(active_actions_.size())) {
    return;
  }
  const ContextMenuAction action = active_actions_[static_cast<size_t>(index)];

  ui::Context* action_context = menu_context_ ? menu_context_ : context_;
  // Copy run callback before deferred dismiss clears active_actions_.
  const std::function<void()> run = action.run;
  on_dismissed_ = nullptr; // chosen, not dismissed
  RequestDismiss();

  if (run) {
    run();
  }
  if (action_context) {
    if (ui::SelectionController* selection = action_context->GetSelectionController()) {
      selection->FinalizeSelection();
      selection->OnPointerUp();
    }
  }
}

void ContextMenuHost::ApplyExpandedSheetHeight() {
  if (!panel_ || !context_) {
    return;
  }
  const ui::Vector2i dims = context_->GetDimensions();
  char height_buf[32];
  std::snprintf(height_buf, sizeof(height_buf), "%.0fpx", std::max(0.f, dims.y * kSheetLargeShare - BottomInsetPx()));
  panel_->SetProperty("height", height_buf);
}

void ContextMenuHost::ExpandSheet(const int index) {
  if (index < 0 || index >= static_cast<int>(active_actions_.size()) || !panel_ || !context_ || sheet_expanded_) {
    return;
  }
  ui::Element* mount = panel_->GetElementById("context-menu-sheet-expand");
  const auto expand = active_actions_[static_cast<size_t>(index)].expand;
  if (!mount || !expand) {
    return;
  }
  sheet_expanded_ = true;
  // The rows and the reactions give way to the action's body; the sheet stands at the large stop, its top edge where
  // a page sheet's is.
  panel_->SetClass("context-menu-sheet--expanded", true);
  ApplyExpandedSheetHeight();
  expand(mount, [this]() {
    on_dismissed_ = nullptr; // chosen, not dismissed
    RequestDismiss();
  });
}

void ContextMenuHost::EndSheetDrag() {
  if (sheet_dragging_ && context_) {
    context_->SetScrollOverscrollEdges(true, true, true, true);
  }
  sheet_tracking_ = false;
  sheet_dragging_ = false;
}

bool ContextMenuHost::HandleSheetEvent(ui::Event& event) {
  ui::Element* target = event.GetTargetElement();
  const auto now = std::chrono::steady_clock::now();
  const auto inside_panel = [this](ui::Element* node) {
    for (; node && node != layer_; node = node->GetParentNode()) {
      if (node == panel_) {
        return true;
      }
    }
    return false;
  };
  switch (event.GetId()) {
    case ui::EventId::Mousedown: {
      sheet_swallow_click_ = false;
      if (!panel_ || !inside_panel(target)) {
        return false; // the scrim: closes as before
      }
      for (ui::Element* node = target; node && node != panel_; node = node->GetParentNode()) {
        if (node->GetId() == "context-menu-cancel") {
          return false; // the close disc: closes at once
        }
      }
      sheet_tracking_ = true;
      sheet_dragging_ = false;
      sheet_press_target_ = target;
      sheet_press_x_ = event.GetParameter<int>("mouse_x", 0);
      sheet_press_y_ = event.GetParameter<int>("mouse_y", 0);
      sheet_last_y_ = sheet_prev_y_ = sheet_press_y_;
      sheet_last_at_ = sheet_prev_at_ = now;
      event.StopPropagation();
      return true;
    }
    case ui::EventId::Mousemove: {
      if (!sheet_tracking_ || !panel_) {
        return true;
      }
      const int x = event.GetParameter<int>("mouse_x", 0);
      const int y = event.GetParameter<int>("mouse_y", 0);
      const int dx = x - sheet_press_x_;
      const int dy = y - sheet_press_y_;
      if (!sheet_dragging_) {
        if (std::abs(dx) <= kSheetDeadzonePx && std::abs(dy) <= kSheetDeadzonePx) {
          return true;
        }
        // Moved: whatever happens next, the release is no tap on a row.
        sheet_swallow_click_ = true;
        sheet_swallow_since_ = now;
        bool list_at_top = true;
        for (ui::Element* node = sheet_press_target_; node && node != panel_; node = node->GetParentNode()) {
          if (node->GetScrollTop() > 1.f) {
            list_at_top = false;
          }
        }
        if (dy <= 0 || std::abs(dx) > std::abs(dy) || !list_at_top) {
          sheet_tracking_ = false; // up, sideways, or the rows scroll back up first
          return true;
        }
        sheet_dragging_ = true;
        if (context_) {
          // The rows must not rubber-band while the sheet itself follows the finger.
          context_->SetScrollOverscrollEdges(true, true, false, true);
          context_->ClearScrollOverscroll();
        }
      }
      sheet_prev_y_ = sheet_last_y_;
      sheet_prev_at_ = sheet_last_at_;
      sheet_last_y_ = y;
      sheet_last_at_ = now;
      char buffer[48];
      std::snprintf(buffer, sizeof(buffer), "translateY(%dpx)", std::max(0, dy));
      panel_->SetClass("context-menu-sheet--dragging", true);
      panel_->SetProperty("transform", buffer);
      event.StopPropagation();
      return true;
    }
    case ui::EventId::Mouseup: {
      if (!sheet_tracking_) {
        return true;
      }
      const bool dragged = sheet_dragging_;
      EndSheetDrag();
      if (!dragged || !panel_) {
        return true;
      }
      sheet_swallow_click_ = true;
      sheet_swallow_since_ = now;
      const float dp = context_ ? context_->GetDensityIndependentPixelRatio() : 1.f;
      const float drag_px = static_cast<float>(sheet_last_y_ - sheet_press_y_);
      const float height_px = panel_->GetBox().GetSize(ui::BoxArea::Border).y;
      const double seconds = ToSeconds(sheet_last_at_ - sheet_prev_at_);
      const bool recent = ToSeconds(now - sheet_last_at_) < 0.1;
      const float speed_dp =
          seconds > 0.0 && recent ? static_cast<float>((sheet_last_y_ - sheet_prev_y_) / seconds) / dp : 0.f;
      panel_->SetClass("context-menu-sheet--dragging", false);
      if (drag_px >= height_px * kSheetDismissShare ||
          (drag_px >= kSheetFlickMinDp * dp && speed_dp >= kSheetFlickDpPerS)) {
        RequestDismiss(/*restore_focus=*/true);
      } else {
        panel_->SetProperty("transform", "translateY(0px)");
      }
      return true;
    }
    case ui::EventId::Click: {
      if (sheet_swallow_click_) {
        sheet_swallow_click_ = false;
        if (now - sheet_swallow_since_ < kSheetSwallowWindow) {
          event.StopPropagation();
          return true;
        }
      }
      if (!target || !inside_panel(target)) {
        return false;
      }
      for (ui::Element* node = target; node && node != panel_; node = node->GetParentNode()) {
        if (node->GetId() == "context-menu-cancel") {
          return false;
        }
        if (node->GetId() == "context-menu-sheet-expand") {
          return true; // the expanded body's own buttons take their clicks
        }
      }
      const int index = FindMenuItemIndex(target);
      if (index >= 0 && index < static_cast<int>(active_actions_.size()) &&
          active_actions_[static_cast<size_t>(index)].expand) {
        ExpandSheet(index);
      } else if (index >= 0) {
        HandleMenuAction(index);
      }
      event.StopPropagation();
      return true;
    }
    default:
      return false;
  }
}

void ContextMenuHost::ProcessEvent(ui::Event& event) {
  if (release_guard_document_) {
    switch (event.GetId()) {
      case ui::EventId::Mousedown:
        // A new press: the long press's finger is gone already (cancelled), nothing left to swallow.
        release_guard_swallow_click_ = false;
        release_guard_done_ = true;
        if (ui::Element* taker = held_focus_taker_.get()) {
          taker->RemoveProperty("focus"); // this press picks its focus as ever
        }
        held_focus_taker_ = nullptr;
        return;
      case ui::EventId::Mouseup:
        release_guard_done_ = true; // the click of this lift follows at once; Update() ends the guard after it
        return;
      case ui::EventId::Click:
        if (release_guard_swallow_click_) {
          release_guard_swallow_click_ = false;
          event.StopPropagation();
          return;
        }
        break;
      default:
        break;
    }
  }
  if (!layer_ || dismiss_pending_) {
    return;
  }

  if (presentation_ == Presentation::ActionSheet && HandleSheetEvent(event)) {
    return;
  }

  const ui::EventId event_id = event.GetId();
  if (event_id != ui::EventId::Mousedown && event_id != ui::EventId::Click) {
    return;
  }

  ui::Element* target = event.GetTargetElement();
  if (!target) {
    return;
  }

  if (target->GetId() == "context-menu-scrim" || target->GetId() == "context-menu-cancel") {
    RequestDismiss(/*restore_focus=*/true);
    event.StopPropagation();
    return;
  }

  // Cancel may be hit via a child text node path; walk up for the cancel id.
  for (ui::Element* node = target; node && node != layer_; node = node->GetParentNode()) {
    if (node->GetId() == "context-menu-cancel") {
      RequestDismiss(/*restore_focus=*/true);
      event.StopPropagation();
      return;
    }
  }

  const int index = FindMenuItemIndex(target);
  if (index < 0) {
    return;
  }

  if (event_id == ui::EventId::Mousedown) {
    HandleMenuAction(index);
    event.StopPropagation();
  }
}

} // namespace pbr
