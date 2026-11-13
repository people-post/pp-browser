#pragma once

#include "gui/shell/SessionSwipeModel.h"

#include <ui/base/ObserverPtr.h>
#include <ui/dom/EventListener.h>

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace ui {
class Context;
class Element;
class ElementDocument;
class Event;
} // namespace ui

namespace pbr {

/**
 * Where a swipe list is and how its rows look to the gesture. The defaults are the phone Chats lists (the Chats page,
 * the AI page's list, the AI drawer's): `sidebar-session` rows with a `session-id`, 74dp buttons, three on the right
 * and two on the left. Another list (New chat's contacts) gives its own.
 */
struct SessionSwipeGestureConfig {
  /** Ids of the elements that hold the rows: a row counts only inside one of them. */
  std::vector<std::string> list_ids = {"sidebar-sessions", "home-ai-sessions", "ai-drawer-sessions"};
  std::string row_class = "sidebar-session";
  /** The row's attribute that names it (given back to the action callback). */
  std::string id_attribute = "session-id";
  /** Button width and counts per side, and the dead zone (SessionSwipeModel). */
  SessionSwipeConfig swipe;
};

/**
 * The phone New chat page's contact rows (new_chat.rml): `contacts-row` rows with a `contact-id`, four 60dp buttons
 * on the right (Info, Voice, Video, More), none on the left. Not the Chats ones, so a contact row is never taken for a
 * chat and a chat row never for a contact.
 */
SessionSwipeGestureConfig ContactRowSwipeConfig();

/**
 * Phone Chats list: iOS-style swipe actions on a row. One set of listeners on the nav page serves every row
 * under `#sidebar-sessions` (rows are rebuilt by the data model, so nothing is bound per row). Left swipe reveals
 * More / Archive / Delete, right swipe Pin / Read; the buttons live inside each row (sidebar.rml). The decisions are in
 * SessionSwipeModel. UI thread; elements are held only as observers.
 */
class SessionSwipeGesture : public ui::EventListener {
public:
  using ActionCallback = std::function<void(const std::string& thread_id, SessionSwipeAction action)>;
  /** The row's id and the button's `swipe-action` text as written in the markup. */
  using NamedActionCallback = std::function<void(const std::string& row_id, const std::string& action)>;

  explicit SessionSwipeGesture(SessionSwipeGestureConfig config = {});

  void Attach(ui::Element* page, ui::Context* context, ActionCallback on_action);
  /** Attach for a list whose buttons are not the Chats ones: their `swipe-action` names go back as they are. */
  void AttachNamed(ui::Element* page, ui::Context* context, NamedActionCallback on_action);
  void Detach();
  bool Attached() const { return static_cast<bool>(page_); }

  void ProcessEvent(ui::Event& event) override;

private:
  ui::Element* RowOf(ui::Element* target) const;
  ui::Element* ButtonOf(ui::Element* target) const;
  void SetRowOffset(ui::Element* row, float dx_dp, bool animate);
  void CloseRow(ui::ObserverPtr<ui::Element>& row);
  void SetDragCapture(bool enabled);
  void Run(const std::string& row_id, const std::string& action);

  ui::ObserverPtr<ui::Element> page_;
  ui::ObserverPtr<ui::Element> document_;
  ui::ObserverPtr<ui::Element> active_row_;
  ui::ObserverPtr<ui::Element> open_row_;
  ui::Context* context_ = nullptr;
  NamedActionCallback on_action_;
  SessionSwipeGestureConfig config_;
  SessionSwipeModel model_;
  bool capture_ = false;
  bool swallow_click_ = false;
  std::chrono::steady_clock::time_point swallow_click_since_{};
};

} // namespace pbr
