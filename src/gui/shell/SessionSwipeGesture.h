#pragma once

#include "gui/shell/SessionSwipeModel.h"

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

#include <chrono>
#include <functional>
#include <string>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
} // namespace ui

namespace pbr {

/**
 * Phone Chats list: iOS-style swipe actions on a row. One set of listeners on the nav page serves every row
 * under `#sidebar-sessions` (rows are rebuilt by the data model, so nothing is bound per row). Left swipe reveals
 * More / Archive / Delete, right swipe Pin / Read; the buttons live inside each row (sidebar.rml). The decisions are in
 * SessionSwipeModel. UI thread; elements are held only as observers.
 */
class SessionSwipeGesture : public ui::EventListener {
public:
  using ActionCallback = std::function<void(const std::string& thread_id, SessionSwipeAction action)>;

  void Attach(ui::Element* page, ui::Context* context, ActionCallback on_action);
  void Detach();
  bool Attached() const { return static_cast<bool>(page_); }

  void ProcessEvent(ui::Event& event) override;

private:
  ui::Element* RowOf(ui::Element* target) const;
  ui::Element* ButtonOf(ui::Element* target) const;
  void SetRowOffset(ui::Element* row, float dx_dp, bool animate);
  void CloseRow(ui::ObserverPtr<ui::Element>& row);
  void SetDragCapture(bool enabled);
  void Run(const std::string& thread_id, SessionSwipeAction action);

  ui::ObserverPtr<ui::Element> page_;
  ui::ObserverPtr<ui::Element> document_;
  ui::ObserverPtr<ui::Element> active_row_;
  ui::ObserverPtr<ui::Element> open_row_;
  ui::Context* context_ = nullptr;
  ActionCallback on_action_;
  SessionSwipeModel model_;
  bool capture_ = false;
  bool swallow_click_ = false;
  std::chrono::steady_clock::time_point swallow_click_since_{};
};

} // namespace pbr
