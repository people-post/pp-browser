#include "gui/shell/SessionSwipeGesture.h"

#include <ui/dom/Context.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/base/ID.h>

#include <cstdio>

namespace pbr {

namespace {

constexpr const char* kListId = "sidebar-sessions";
constexpr const char* kRowClass = "sidebar-session";
constexpr const char* kDraggingClass = "session-swipe-dragging";

int EventMouseX(const ui::Event& event) {
  return event.GetParameter<int>("mouse_x", 0);
}

int EventMouseY(const ui::Event& event) {
  return event.GetParameter<int>("mouse_y", 0);
}

bool ParseAction(const ui::String& name, SessionSwipeAction& out) {
  if (name == "archive") {
    out = SessionSwipeAction::Archive;
  } else if (name == "delete") {
    out = SessionSwipeAction::Delete;
  } else if (name == "pin") {
    out = SessionSwipeAction::Pin;
  } else if (name == "read") {
    out = SessionSwipeAction::Read;
  } else {
    return false;
  }
  return true;
}

} // namespace

void SessionSwipeGesture::Attach(ui::Element* page, ui::Context* context, ActionCallback on_action) {
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
  model_ = SessionSwipeModel();
  swallow_click_ = false;
}

ui::Element* SessionSwipeGesture::RowOf(ui::Element* target) const {
  ui::Element* row = nullptr;
  for (ui::Element* node = target; node; node = node->GetParentNode()) {
    if (node->IsClassSet(kRowClass) && node->HasAttribute("session-id")) {
      row = node;
    } else if (row && node->GetId() == kListId) {
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
    if (node->IsClassSet(kRowClass)) {
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
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "translateX(%.1fdp)", dx_dp);
  row->SetProperty("transform", buffer);
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

void SessionSwipeGesture::Run(const std::string& thread_id, const SessionSwipeAction action) {
  if (on_action_ && !thread_id.empty()) {
    on_action_(thread_id, action);
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
    SessionSwipeAction action{};
    if (!button || !row || !ParseAction(button->GetAttribute("swipe-action", ui::String()), action)) {
      return;
    }
    event.StopPropagation(); // the button is inside the row: it must not also open the chat
    const std::string thread_id = row->GetAttribute("session-id", ui::String()).c_str();
    model_.CloseOpen();
    CloseRow(open_row_);
    Run(thread_id, action);
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
    const std::string id = row->GetAttribute("session-id", ui::String()).c_str();
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
    case SessionSwipeOutcome::Kind::RunFirst: {
      const std::string thread_id =
          active_row_ ? std::string(active_row_->GetAttribute("session-id", ui::String()).c_str()) : std::string();
      CloseRow(active_row_);
      open_row_ = nullptr;
      // First action on each side: Archive (End) / Pin (Start). Delete asks its usual question afterwards.
      Run(thread_id, out.side == SwipeSide::End ? SessionSwipeAction::Archive : SessionSwipeAction::Pin);
      break;
    }
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
