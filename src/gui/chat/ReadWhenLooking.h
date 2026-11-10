#pragma once

#include "gui/chat/AgentBusyRules.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * The open chat's transcript is on screen. Phone layout: the chat page is open over the list (ChatPageShows).
 * Desktop layout: the primary pane is the chat, whichever tab it was opened from (a click on a desktop
 * notification or on a row of Home's "New messages" keeps the Home tab and shows the chat pane there; ChatPageShows
 * says no for Home, which is right for its other users).
 */
inline bool ChatTranscriptOnScreen(const ShellChromeSnapshot& chrome, const std::string& active_thread,
                                   const std::string& thread_id) {
  if (thread_id.empty() || thread_id != active_thread) {
    return false;
  }
  if (chrome.layout_mode == LayoutMode::Compact) {
    return ChatPageShows(chrome, active_thread, thread_id);
  }
  return chrome.primary_pane_key == "chat";
}

/**
 * A notification tap is waiting to be routed (PendingNotificationTap) to somewhere other than the open chat: a
 * banner tap of another chat, or a message / call alert tap whose chat is not known until it syncs. The app coming
 * to the front for that tap must not read the chat it happens to have open: the route opens the tapped one next.
 */
inline bool TapPendingElsewhere(const std::optional<std::string>& pending_thread, bool pending_route_unknown_thread,
                                const std::string& open_thread) {
  if (pending_thread && !pending_thread->empty()) {
    return *pending_thread != open_thread;
  }
  return pending_route_unknown_thread;
}

/**
 * Whether the open chat is marked read now: the user is looking at the app (AppLifecycle::IsUserAttentive()), a chat
 * is open, its transcript is on screen (ChatTranscriptOnScreen), it has unread messages, and no notification tap is
 * about to open another chat (TapPendingElsewhere). A message that came in while nobody was looking counts as
 * unread even in the open chat; this is what reads it when the person is back.
 */
inline bool ShouldMarkOpenChatRead(bool attentive, bool has_open_chat, bool chat_on_screen, bool has_unread,
                                   bool tap_pending_elsewhere) {
  return attentive && has_open_chat && chat_on_screen && has_unread && !tap_pending_elsewhere;
}

} // namespace pbr
