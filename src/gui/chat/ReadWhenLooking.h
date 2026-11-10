#pragma once

namespace pbr {

/**
 * Whether the open chat is marked read now: the user is looking at the app (AppLifecycle::IsUserAttentive()), a chat
 * is open, its page is on screen (ChatPageShows) and it has unread messages. A message that came in while nobody was
 * looking counts as unread even in the open chat; this is what reads it when the person is back.
 */
inline bool ShouldMarkOpenChatRead(bool attentive, bool has_open_chat, bool chat_page_shows, bool has_unread) {
  return attentive && has_open_chat && chat_page_shows && has_unread;
}

} // namespace pbr
