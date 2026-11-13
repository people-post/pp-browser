#include "gui/shell/SessionSwipeGesture.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/base/ID.h>

#include <algorithm>
#include <cstdio>

namespace pbr {

namespace {

// The default lists and rows are in SessionSwipeGestureConfig: the Chats page, the PP助手 page's AI conversations list
// and the phone AI drawer's list (ai_drawer.rml; DEV/PLAN-minimal-nav.md C).
constexpr const char* kDraggingClass = "session-swipe-dragging";
// Set while the row is moved off its rest position: the buttons outside the row are only drawn then, so they never
// peek into the strip the list keeps on its right edge.
constexpr const char* kShownClass = "session-swipe-shown";

int EventMouseX(const ui::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const ui::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

bool ParseAction(const ui::String& name, SessionSwipeAction& out) {
  if (name == "more") {
    out = SessionSwipeAction::More;
  } else if (name == "archive") {
    out = SessionSwipeAction::Archive;
  } else if (name == "delete") {
    out = SessionSwipeAction::Delete;
  } else if (name == "pin") {
    out = SessionSwipeAction::Pin;
  } else if (name == "read") {
    out = SessionSwipeAction::Read;
  } else if (name == "rename") {
    out = SessionSwipeAction::Rename;
  } else {
    return false;
  }
  return true;
}

} // namespace

SessionSwipeGestureConfig ContactRowSwipeConfig() {
  SessionSwipeGestureConfig config;
  config.list_ids = {"new-chat-list"};
  config.row_class = "contacts-row";
  config.id_attribute = "contact-id";
  config.swipe.button_dp = 60.f;
  config.swipe.end_buttons = 4;
  config.swipe.start_buttons = 0;
  return config;
}

SessionSwipeGesture::SessionSwipeGesture(SessionSwipeGestureConfig config)
    : config_(std::move(config)), model_(config_.swipe) {}

void SessionSwipeGesture::Attach(ui::Element* page, ui::Context* context, ActionCallback on_action) {
  AttachNamed(page, context, [on_action = std::move(on_action)](const std::string& row_id, const std::string& name) {
    SessionSwipeAction action{};
    if (on_action && ParseAction(name.c_str(), action)) {
      on_action(row_id, action);
    }
  });
}

void SessionSwipeGesture::AttachNamed(ui::Element* page, ui::Context* context, NamedActionCallback on_action) {
  Detach();
  if (!page || !context) {
    return;
  }
  page_ = page->GetObserverPtr();
  document_ = page->GetOwnerDocument() ? page->GetOwnerDocument()->GetObserverPtr() : ui::ObserverPtr<ui::Element>();
  context_ = context;
  on_action_ = std::move(on_action);
  page->AddEventListener(ui::EventId::Mousedown, this);
  page->AddEventListener(ui::EventId::Click, this, true);
  page->AddEventListener(ui::EventId::Scroll, this);
}

void SessionSwipeGesture::Detach() {
  SetDragCapture(false);
  if (active_row_) {
    SetRowOffset(active_row_.get(), 0.f, false);
  }
  if (open_row_) {
    SetRowOffset(open_row_.get(), 0.f, false);
  }
  if (page_) {
    page_->RemoveEventListener(ui::EventId::Mousedown, this);
    page_->RemoveEventListener(ui::EventId::Click, this, true);
    page_->RemoveEventListener(ui::EventId::Scroll, this);
  }
  page_ = nullptr;
  document_ = nullptr;
  active_row_ = nullptr;
  open_row_ = nullptr;
  context_ = nullptr;
  on_action_ = nullptr;
  model_ = SessionSwipeModel(config_.swipe);
  swallow_click_ = false;
}

ui::Element* SessionSwipeGesture::RowOf(ui::Element* target) const {
  ui::Element* row = nullptr;
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node->IsClassSet(config_.row_class.c_str()) && node->HasAttribute(config_.id_attribute.c_str())) {
      row = node;
    } else if (row && std::find(config_.list_ids.begin(), config_.list_ids.end(), std::string(node->GetId().c_str())) !=
                          config_.list_ids.end()) {
      return row;
    }
  }
  return nullptr;
}

ui::Element* SessionSwipeGesture::ButtonOf(ui::Element* target) const {
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node->HasAttribute("swipe-action")) {
      return node;
    }
    if (node->IsClassSet(config_.row_class.c_str())) {
      break;
    }
  }
  return nullptr;
}

void SessionSwipeGesture::SetRowOffset(ui::Element* row, float dx_dp, bool animate) {
  if (!row) {
    return;
  }
  row->SetClass(kDraggingClass, !animate);
  row->SetClass(kShownClass, dx_dp != 0.f);
  // `left` on the relatively positioned row, not a transform: the engine hit-tests a transformed row's children at
  // their untransformed place, clipped by the list, so the revealed buttons never got a tap (it fell through to the
  // list, and the row sliding back under the finger took the click and opened the chat).
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.1fdp", dx_dp);
  row->SetProperty("left", buffer);
}

void SessionSwipeGesture::CloseRow(ui::ObserverPtr<ui::Element>& row) {
  if (row) {
    SetRowOffset(row.get(), 0.f, true);
  }
  row = nullptr;
}

void SessionSwipeGesture::SetDragCapture(bool enabled) {
  if (!document_ || capture_ == enabled) {
    return;
  }
  if (enabled) {
    document_->AddEventListener(ui::EventId::Mousemove, this, true);
    document_->AddEventListener(ui::EventId::Mouseup, this, true);
  } else {
    document_->RemoveEventListener(ui::EventId::Mousemove, this, true);
    document_->RemoveEventListener(ui::EventId::Mouseup, this, true);
  }
  capture_ = enabled;
}

void SessionSwipeGesture::Run(const std::string& row_id, const std::string& action) {
  if (on_action_ && !row_id.empty()) {
    // A copy: the action may release this gesture (its page goes away) while it still runs.
    const NamedActionCallback run = on_action_;
    run(row_id, action);
  }
}

void SessionSwipeGesture::ProcessEvent(ui::Event& event) {
  if (!page_ || !context_) {
    return;
  }
  const float dpr = context_->GetDensityIndependentPixelRatio();

  switch (event.GetId()) {
    case ui::EventId::Scroll:
      // Scrolling the list closes the open row.
      if (!model_.OpenRow().empty()) {
        model_.CloseOpen();
        CloseRow(open_row_);
      }
      break;
    case ui::EventId::Click: {
      if (swallow_click_) {
        // Only the click of the same pointer sequence; a later keyboard / programmatic click goes through.
        const bool same_sequence =
            std::chrono::steady_clock::now() - swallow_click_since_ < std::chrono::milliseconds(300);
        swallow_click_ = false;
        if (same_sequence) {
          event.StopPropagation();
          return;
        }
      }
      ui::Element* target = event.GetTargetElement();
      ui::Element* button = ButtonOf(target);
      ui::Element* row = button ? RowOf(button) : nullptr;
      if (!button || !row) {
        return;
      }
      const std::string action = button->GetAttribute("swipe-action", ui::String()).c_str();
      event.StopPropagation(); // the button is inside the row: it must not also open the chat
      const std::string row_id = row->GetAttribute(config_.id_attribute.c_str(), ui::String()).c_str();
      model_.CloseOpen();
      CloseRow(open_row_);
      Run(row_id, action);
      break;
    }
    case ui::EventId::Mousedown: {
      swallow_click_ = false;
      ui::Element* target = event.GetTargetElement();
      if (ButtonOf(target)) {
        return; // a revealed button: its click does the work, and must not close the row first
      }
      ui::Element* row = RowOf(target);
      if (!row) {
        // Tap elsewhere closes the open row.
        if (!model_.OpenRow().empty()) {
          model_.CloseOpen();
          CloseRow(open_row_);
        }
        return;
      }
      const float ratio = dpr > 0.f ? dpr : 1.f;
      const float width_dp = row->GetBox().GetSize(ui::BoxArea::Border).x / ratio;
      const std::string id = row->GetAttribute(config_.id_attribute.c_str(), ui::String()).c_str();
      const std::string closed = model_.Press(id, EventMouseX(event), EventMouseY(event), width_dp);
      if (!closed.empty()) {
        CloseRow(open_row_);
      }
      active_row_ = row->GetObserverPtr();
      SetDragCapture(true);
      break;
    }
    case ui::EventId::Mousemove: {
      const SessionSwipeModel::Move move = model_.Update(EventMouseX(event), EventMouseY(event), dpr);
      if (move.claimed && active_row_) {
        SetRowOffset(active_row_.get(), move.offset_dp, false);
        event.StopPropagation();
      }
      break;
    }
    case ui::EventId::Mouseup: {
      SetDragCapture(false);
      const SessionSwipeOutcome out = model_.Release();
      if (out.swallow_click) {
        swallow_click_ = true;
        swallow_click_since_ = std::chrono::steady_clock::now();
      }
      switch (out.kind) {
        case SessionSwipeOutcome::Kind::Open:
          SetRowOffset(active_row_.get(), model_.RestOffsetDp(out.side), true);
          open_row_ = active_row_;
          break;
        case SessionSwipeOutcome::Kind::Close:
          CloseRow(active_row_);
          open_row_ = nullptr;
          break;
        case SessionSwipeOutcome::Kind::None:
          break;
      }
      active_row_ = nullptr;
      break;
    }
    default:
      break;
  }
}

} // namespace pbr
