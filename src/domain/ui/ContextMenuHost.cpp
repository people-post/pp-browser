#include "domain/ui/ContextMenuHost.h"

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
#include <cstdio>
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

ui::Vector2i MenuPositionBelowEvent(ui::Event& ev, float gap_px) {
  return MenuPositionBelow(EventAnchorElement(ev), gap_px);
}

ui::Vector2i MenuPositionBelowRightAlignedEvent(ui::Event& ev, float menu_min_width_px, float gap_px) {
  return MenuPositionBelowRightAligned(EventAnchorElement(ev), menu_min_width_px, gap_px);
}

namespace {

constexpr float kViewportMarginPx = 8.f;
constexpr float kActionSheetInsetDp = 12.f;
constexpr float kActionSheetBottomDp = 20.f;
constexpr float kActionSheetMaxWidthDp = 480.f;

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

void AppendActionButtons(std::ostringstream& out, const std::vector<ContextMenuAction>& actions) {
  for (size_t i = 0; i < actions.size(); ++i) {
    const ContextMenuAction& action = actions[i];
    const bool enabled = !action.enabled || action.enabled();
    if (!enabled) {
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
      out << "<div class=\"context-menu-item-icon\"><svg src=\"" << action.icon
          << "\" width=\"16\" height=\"16\" crop-to-content=\"true\"></svg></div>";
    }
    out << "<span class=\"context-menu-item-label\">" << action.label << "</span>";
    if (action.selected) {
      out << "<span class=\"context-menu-item-check\">✓</span>";
    }
    out << "</button>";
  }
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
  context_->SetTouchLongPressCallback([](ui::Vector2i position, ui::Element* target) {
    ContextMenuHost::Instance().OnLongPress(position, target);
  });
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
  ShowAt(request);
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

  actions.push_back({"copy", "Copy", copy_enabled, copy_run});
  actions.push_back({"select_all", "Select All", select_all_enabled, select_all_run});
  actions.push_back({"paste", "Paste", paste_enabled, paste_run});
  return actions;
}

std::vector<ContextMenuAction> ContextMenuHost::CollectActions(const ContextMenuRequest& request) const {
  std::vector<ContextMenuAction> actions = BuildTextActions();
  for (const auto& provider : providers_) {
    if (!provider) {
      continue;
    }
    const auto provided = provider(request);
    actions.insert(actions.end(), provided.begin(), provided.end());
  }
  return actions;
}

void ContextMenuHost::ClampFloatPanel(ui::Vector2i preferred) {
  if (!panel_ || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }

  ui::ElementDocument* document = context_->GetDocument(0);
  document->UpdateDocument();

  const ui::Vector2i dims = context_->GetDimensions();
  const ui::Vector2f size = panel_->GetBox().GetSize(ui::BoxArea::Border);
  if (size.x <= 0.f || size.y <= 0.f || dims.x <= 0 || dims.y <= 0) {
    return;
  }

  float left = static_cast<float>(preferred.x);
  float top = static_cast<float>(preferred.y);
  const float max_left = static_cast<float>(dims.x) - size.x - kViewportMarginPx;
  const float max_top = static_cast<float>(dims.y) - size.y - kViewportMarginPx;

  if (left > max_left) {
    left = max_left;
  }
  if (left < kViewportMarginPx) {
    left = kViewportMarginPx;
  }

  if (top + size.y > static_cast<float>(dims.y) - kViewportMarginPx) {
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

void ContextMenuHost::LayoutActionSheet() {
  if (!panel_ || !context_) {
    return;
  }

  const ui::Vector2i dims = context_->GetDimensions();
  if (dims.x <= 0 || dims.y <= 0) {
    return;
  }

  const float dp = context_->GetDensityIndependentPixelRatio();
  const float inset = kActionSheetInsetDp * dp;
  const float bottom = kActionSheetBottomDp * dp;
  const float max_width = kActionSheetMaxWidthDp * dp;
  float width = static_cast<float>(dims.x) - inset * 2.f;
  if (width > max_width) {
    width = max_width;
  }
  if (width < 1.f) {
    width = 1.f;
  }
  const float left = (static_cast<float>(dims.x) - width) * 0.5f;

  char left_buf[32];
  char width_buf[32];
  char bottom_buf[32];
  std::snprintf(left_buf, sizeof(left_buf), "%.0fpx", left);
  std::snprintf(width_buf, sizeof(width_buf), "%.0fpx", width);
  std::snprintf(bottom_buf, sizeof(bottom_buf), "%.0fpx", bottom);
  // Viewport frame only; children stretch via RCSS width: 100%.
  panel_->SetProperty("left", left_buf);
  panel_->SetProperty("right", "auto");
  panel_->SetProperty("width", width_buf);
  panel_->SetProperty("bottom", bottom_buf);
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
    out << "<div class=\"context-menu-sheet\" id=\"context-menu-panel\">";
    out << "<div class=\"context-menu-sheet-list\" id=\"context-menu-sheet-list\">";
    AppendActionButtons(out, actions);
    out << "</div>";
    out << "<button class=\"context-menu-sheet-cancel\" type=\"button\" id=\"context-menu-cancel\">"
        << Tr("common.cancel") << "</button>";
    out << "</div>";
  } else {
    out << "<div class=\"context-menu-panel\" id=\"context-menu-panel\" style=\"left: " << request.position.x
        << "px; top: " << request.position.y << "px;\">";
    AppendActionButtons(out, actions);
    out << "</div>";
  }

  ui::ElementPtr layer_element = document->CreateElement("div");
  layer_element->SetAttribute("id", "context-menu-layer");
  layer_element->SetClass("context-menu-layer", true);
  if (presentation == Presentation::ActionSheet) {
    layer_element->SetClass("context-menu-layer--sheet", true);
  }
  layer_element->SetInnerRML(out.str());
  layer_ = body->AppendChild(std::move(layer_element));
  panel_ = layer_ ? layer_->GetElementById("context-menu-panel") : nullptr;
  if (!layer_) {
    return;
  }
  layer_->AddEventListener(ui::EventId::Mousedown, this, true);
  layer_->AddEventListener(ui::EventId::Click, this, true);

  if (presentation == Presentation::Float) {
    ClampFloatPanel(request.position);
  } else {
    LayoutActionSheet();
  }
}

void ContextMenuHost::ShowAt(const ContextMenuRequest& request) {
  Dismiss();
  menu_context_ = request.context ? request.context : context_;
  menu_target_ = request.target;
  menu_editor_ = nullptr;
  copy_snapshot_.clear();
  if (menu_context_) {
    ui::Element* focus = menu_context_->GetFocusElement();
    menu_editor_ = FindTextEditor(focus ? focus : menu_target_);
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
    return;
  }
  // Contextual menus stay anchored near the pointer/selection even on compact layout.
  RenderMenu(request, active_actions_, Presentation::Float);
}

void ContextMenuHost::ShowActions(ui::Vector2i position, std::vector<ContextMenuAction> actions) {
  Dismiss();
  if (!context_ || actions.empty()) {
    return;
  }

  menu_context_ = context_;
  menu_target_ = nullptr;
  menu_editor_ = nullptr;
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
    return;
  }

  ContextMenuRequest request;
  request.position = position;
  request.context = context_;
  const Presentation presentation =
      compact_layout_ ? Presentation::ActionSheet : Presentation::Float;
  RenderMenu(request, active_actions_, presentation);
}

void ContextMenuHost::Dismiss() {
  dismiss_pending_ = false;
  // Capture and clear before RemoveEventListener: DetachEvent invokes OnDetach, which
  // would otherwise null layer_ mid-function and crash the second RemoveEventListener.
  ui::Element* layer = layer_;
  layer_ = nullptr;
  panel_ = nullptr;
  presentation_ = Presentation::Float;
  if (layer) {
    layer->RemoveEventListener(ui::EventId::Mousedown, this, true);
    layer->RemoveEventListener(ui::EventId::Click, this, true);
    if (ui::Element* parent = layer->GetParentNode()) {
      parent->RemoveChild(layer);
    }
  }
  active_actions_.clear();
  menu_context_ = nullptr;
  menu_target_ = nullptr;
  menu_editor_ = nullptr;
  copy_snapshot_.clear();
}

void ContextMenuHost::RequestDismiss() {
  // Defer DOM removal until after the current pointer event finishes. Removing the
  // hover/active target mid-mousedown leaves dangling Context::active pointers.
  dismiss_pending_ = true;
}

void ContextMenuHost::Update() {
  if (!dismiss_pending_) {
    return;
  }
  Dismiss();
}

bool ContextMenuHost::HandleDismiss() {
  if (!IsOpen() && !dismiss_pending_) {
    return false;
  }
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

void ContextMenuHost::ProcessEvent(ui::Event& event) {
  if (!layer_ || dismiss_pending_) {
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
    RequestDismiss();
    event.StopPropagation();
    return;
  }

  // Cancel may be hit via a child text node path; walk up for the cancel id.
  for (ui::Element* node = target; node && node != layer_; node = node->GetParentNode()) {
    if (node->GetId() == "context-menu-cancel") {
      RequestDismiss();
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
