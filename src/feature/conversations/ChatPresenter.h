#pragma once

#include "common/Module.h"
#include "feature/conversations/SessionMenu.h"
#include "lib/messaging/ThreadLocalPrefsStore.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsFacade;
struct Thread;

/** One row of the chats list, as the user sees it (no RmlUi types). */
struct ChatSessionRow {
  std::string id;
  std::string title;
  /** The title is the stored default of a new AI chat: the renderer shows it in the UI language. */
  bool title_is_default_ai = false;
  std::string preview;
  /** Visual chat type: ai | private | public | group. */
  std::string kind;
  int unread_count = 0;
  /** Marked unread by hand with no real unread count: a dot, not a number. */
  bool unread_dot = false;
  bool pinned = false;
  bool muted = false;
  std::string date_label;
  bool active = false;

  bool operator==(const ChatSessionRow&) const = default;
};

/** The chats list: pinned first, then by activity; archived chats are a separate list. */
struct ChatListScreen {
  std::vector<ChatSessionRow> rows;
  int archived_count = 0;
  /** The archived list is showing (back to the main list once nothing is archived). */
  bool showing_archived = false;

  bool operator==(const ChatListScreen&) const = default;
};

/** Visual chat type of a thread: ai | private | public | group. */
std::string ChatSessionKindName(const Thread& thread);

/**
 * The chats list's decisions and its buttons, without a GUI: which chats show and how (order,
 * unread, pin, mute, archive), and what opening one or changing its settings does. ChatController
 * renders the list (words, menus); pp-app-probe presses the same buttons and reports the same list,
 * so the app lab checks what people see. The pattern is CallPresenter's.
 *
 * UI thread.
 */
class ChatPresenter : public Module {
public:
  struct Callbacks {
    /** The list may have changed: re-read List(). */
    std::function<void()> on_changed;
    /** A settings write the store refused (the renderer tells the user). */
    std::function<void()> prefs_failed;
  };

  ChatPresenter();

  /** The messaging facade, or null while messaging is not ready (the list is then empty). */
  void BindFacade(ConversationsFacade* facade);
  void BindCallbacks(Callbacks callbacks);
  /** The profile's data was reset: nothing read from the old store may be written to the new one. */
  void ResetForNewProfile();

  /** Rebuild the list from the threads and their settings. */
  void Refresh();
  const ChatListScreen& List() const { return list_; }

  // --- buttons ----------------------------------------------------------------------------------

  /** Open a chat: it is read (a "marked unread" dot goes too), its notification is cleared. */
  bool SelectThread(const std::string& thread_id);
  /** Back to the chats list (the Home tab): no chat is open, so new messages count as unread. */
  void ShowHome();
  /** The archived list's entry / back button. */
  void ToggleArchived();
  void SetPinned(const std::string& thread_id, bool pinned);
  void Mute(const std::string& thread_id, SessionMuteChoice choice);
  void Unmute(const std::string& thread_id);
  void SetArchived(const std::string& thread_id, bool archived);
  void MarkUnread(const std::string& thread_id);
  void MarkRead(const std::string& thread_id);
  /** A chat was closed / deleted: forget its settings. */
  void ForgetThread(const std::string& thread_id);

  /** One chat's settings (defaults when it has none); nullopt when the store cannot be read. */
  std::optional<ThreadLocalPrefs> PrefsOf(const std::string& thread_id);

private:
  /**
   * Pin / mute / archive / marked-unread of every thread that has any set: read once when the store
   * can be listed, then kept in step with every write. Empty, and read again next time, while it
   * cannot.
   */
  const std::unordered_map<std::string, ThreadLocalPrefs>& AllPrefs();
  /** Writes the settings and the cache only; false when the store refused. */
  bool StorePrefs(const ThreadLocalPrefs& prefs);
  /** Reads, changes and writes one chat's settings, then refreshes; tells the renderer when refused. */
  void UpdatePrefs(const std::string& thread_id, const std::function<void(ThreadLocalPrefs&)>& change);
  void Changed();

  ConversationsFacade* facade_ = nullptr;
  Callbacks callbacks_;
  ChatListScreen list_;
  bool showing_archived_ = false;
  std::optional<std::unordered_map<std::string, ThreadLocalPrefs>> prefs_;
};

} // namespace pbr
