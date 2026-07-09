#include "base/ui/ContextMenuHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SelectionController.h>
#include <RmlUi/Core/SystemInterface.h>

#include <climits>
#include <cstdio>
#include <sstream>

namespace pbr {

namespace {

Rml::Element* FindTextEditor(Rml::Element* element) {
  for (Rml::Element* node = element; node; node = node->GetParentNode()) {
    const Rml::String& tag = node->GetTagName();
    if (tag == "textarea" || tag == "input") {
      return node;
    }
  }
  return nullptr;
}

std::string GetEditorSelectedText(Rml::Element* editor) {
  if (!editor) {
    return {};
  }
  int start = 0;
  int end = 0;
  Rml::String selected;
  if (auto* textarea = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(editor)) {
    textarea->GetSelection(&start, &end, &selected);
  } else if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(editor)) {
    input->GetSelection(&start, &end, &selected);
  }
  return selected;
}

void SelectAllInEditor(Rml::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  if (auto* textarea = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(editor)) {
    textarea->SetSelectionRange(0, INT_MAX);
  } else if (auto* input = rmlui_dynamic_cast<Rml::ElementFormControlInput*>(editor)) {
    input->SetSelectionRange(0, INT_MAX);
  }
}

void PasteIntoEditor(Rml::Element* editor) {
  if (!editor) {
    return;
  }
  editor->Focus();
  Rml::String clipboard;
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->GetClipboardText(clipboard);
  }
  if (clipboard.empty()) {
    return;
  }
  Rml::Dictionary parameters;
  parameters["text"] = clipboard;
  editor->DispatchEvent(Rml::EventId::Textinput, parameters);
}

} // namespace

ContextMenuHost& ContextMenuHost::Instance() {
  static ContextMenuHost host;
  return host;
}

void ContextMenuHost::Install(Rml::Context* context) {
  context_ = context;
  if (!context_) {
    return;
  }
  context_->SetTouchLongPressCallback([](Rml::Vector2i position, Rml::Element* target) {
    ContextMenuHost::Instance().OnLongPress(position, target);
  });
}

void ContextMenuHost::RegisterProvider(
    std::function<std::vector<ContextMenuAction>(const ContextMenuRequest&)> provider) {
  providers_.push_back(std::move(provider));
}

void ContextMenuHost::OnLongPress(Rml::Vector2i position, Rml::Element* target) {
  if (!context_) {
    return;
  }
  ContextMenuRequest request;
  request.position = position;
  request.target = target;
  request.context = context_;
  ShowAt(request);
}

bool ContextMenuHost::OnContextPointer(Rml::Context* context, int x, int y) {
  if (!context) {
    return false;
  }
  const Rml::Vector2f point(static_cast<float>(x), static_cast<float>(y));
  Rml::Element* target = context->GetElementAtPoint(point);
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
  Rml::Context* context = menu_context_ ? menu_context_ : context_;
  if (!context) {
    return actions;
  }

  Rml::Element* editor = menu_editor_;
  Rml::Element* target = menu_target_;
  Rml::SelectionController* selection = context->GetSelectionController();
  const std::string snapshot = copy_snapshot_;

  auto copy_enabled = [snapshot]() { return !snapshot.empty(); };
  auto copy_run = [snapshot]() {
    if (snapshot.empty()) {
      return;
    }
    if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
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

void ContextMenuHost::RenderMenu(const ContextMenuRequest& request, const std::vector<ContextMenuAction>& actions) {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  Rml::ElementDocument* document = context_->GetDocument(0);
  Rml::Element* body = document;
  if (Rml::Element* shell_root = document->GetElementById("shell-root")) {
    if (Rml::Element* shell_body = shell_root->GetParentNode()) {
      body = shell_body;
    }
  }
  if (!body) {
    return;
  }

  std::ostringstream out;
  out << "<div id=\"context-menu-layer\" class=\"context-menu-layer\">";
  out << "<div class=\"context-menu-scrim\" id=\"context-menu-scrim\"></div>";
  out << "<div class=\"context-menu-panel\" id=\"context-menu-panel\" style=\"left: " << request.position.x
      << "px; top: " << request.position.y << "px;\">";
  for (size_t i = 0; i < actions.size(); ++i) {
    const ContextMenuAction& action = actions[i];
    const bool enabled = !action.enabled || action.enabled();
    if (!enabled) {
      continue;
    }
    out << "<button class=\"context-menu-item\" type=\"button\" data-item-index=\"" << i << "\">" << action.label
        << "</button>";
  }
  out << "</div>";

  Rml::ElementPtr layer_element = document->CreateElement("div");
  layer_element->SetAttribute("id", "context-menu-layer");
  layer_element->SetClass("context-menu-layer", true);
  layer_element->SetInnerRML(out.str());
  body->AppendChild(std::move(layer_element));
  layer_ = document->GetElementById("context-menu-layer");
  panel_ = document->GetElementById("context-menu-panel");
  if (!layer_) {
    return;
  }
  layer_->AddEventListener(Rml::EventId::Mousedown, this, true);
  layer_->AddEventListener(Rml::EventId::Click, this, true);
}

void ContextMenuHost::ShowAt(const ContextMenuRequest& request) {
  Dismiss();
  menu_context_ = request.context ? request.context : context_;
  menu_target_ = request.target;
  menu_editor_ = nullptr;
  copy_snapshot_.clear();
  if (menu_context_) {
    Rml::Element* focus = menu_context_->GetFocusElement();
    menu_editor_ = FindTextEditor(focus ? focus : menu_target_);
    if (menu_editor_) {
      copy_snapshot_ = GetEditorSelectedText(menu_editor_);
    } else if (Rml::SelectionController* selection = menu_context_->GetSelectionController()) {
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
  RenderMenu(request, active_actions_);
}

void ContextMenuHost::ShowActions(Rml::Vector2i position, std::vector<ContextMenuAction> actions) {
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
  RenderMenu(request, active_actions_);
}

void ContextMenuHost::Dismiss() {
  if (layer_) {
    layer_->RemoveEventListener(Rml::EventId::Mousedown, this, true);
    layer_->RemoveEventListener(Rml::EventId::Click, this, true);
    if (Rml::Element* parent = layer_->GetParentNode()) {
      parent->RemoveChild(layer_);
    }
    layer_ = nullptr;
    panel_ = nullptr;
  }
  active_actions_.clear();
  menu_context_ = nullptr;
  menu_target_ = nullptr;
  menu_editor_ = nullptr;
  copy_snapshot_.clear();
}

bool ContextMenuHost::HandleDismiss() {
  if (!IsOpen()) {
    return false;
  }
  Dismiss();
  return true;
}

int ContextMenuHost::FindMenuItemIndex(Rml::Element* target) const {
  for (Rml::Element* node = target; node; node = node->GetParentNode()) {
    const Rml::Variant* index_variant = node->GetAttribute("data-item-index");
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

  Rml::Context* action_context = menu_context_ ? menu_context_ : context_;
  Dismiss();

  if (action.run) {
    action.run();
  }
  if (action_context) {
    if (Rml::SelectionController* selection = action_context->GetSelectionController()) {
      selection->FinalizeSelection();
      selection->OnPointerUp();
    }
  }
}

void ContextMenuHost::ProcessEvent(Rml::Event& event) {
  if (!layer_) {
    return;
  }

  const Rml::EventId event_id = event.GetId();
  if (event_id != Rml::EventId::Mousedown && event_id != Rml::EventId::Click) {
    return;
  }

  Rml::Element* target = event.GetTargetElement();
  if (!target) {
    return;
  }

  if (target->GetId() == "context-menu-scrim") {
    Dismiss();
    event.StopPropagation();
    return;
  }

  const int index = FindMenuItemIndex(target);
  if (index < 0) {
    return;
  }

  if (event_id == Rml::EventId::Mousedown) {
    HandleMenuAction(index);
    event.StopPropagation();
  }
}

} // namespace pbr
