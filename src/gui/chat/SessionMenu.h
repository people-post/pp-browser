#pragma once

#include <cstdint>
#include <limits>
#include <vector>

namespace pbr {

/** Pure rules for the context menu of a row in the sessions list (no RmlUi, no storage). */

enum class SessionKind { Ai, Direct, Group };

enum class SessionMenuItem {
  MarkUnread,
  MarkRead,
  Pin,
  Unpin,
  Mute,
  Unmute,
  Archive,
  Unarchive,
  ViewContact,
  ClearChat,
  Delete,
  LeaveGroup,
};

struct SessionMenuState {
  SessionKind kind = SessionKind::Ai;
  /** The row shows an unread badge. */
  bool unread = false;
  /** Direct chat whose peer is a saved contact. */
  bool has_contact = false;
  bool pinned = false;
  /** Muted right now (an expired mute is not muted). */
  bool muted = false;
  bool archived = false;
};

/** Destructive items are styled as danger and ask before they act. */
inline bool SessionMenuItemIsDestructive(const SessionMenuItem item) {
  return item == SessionMenuItem::ClearChat || item == SessionMenuItem::Delete || item == SessionMenuItem::LeaveGroup;
}

/**
 * The items for one row, in display order. Items that make no sense for the row are left out rather
 * than disabled; destructive ones come last.
 */
inline std::vector<SessionMenuItem> SessionMenuItems(const SessionMenuState& state) {
  std::vector<SessionMenuItem> items;
  items.push_back(state.unread ? SessionMenuItem::MarkRead : SessionMenuItem::MarkUnread);
  items.push_back(state.pinned ? SessionMenuItem::Unpin : SessionMenuItem::Pin);
  if (state.kind != SessionKind::Ai) { // an AI thread never notifies
    items.push_back(state.muted ? SessionMenuItem::Unmute : SessionMenuItem::Mute);
  }
  items.push_back(state.archived ? SessionMenuItem::Unarchive : SessionMenuItem::Archive);
  if (state.kind == SessionKind::Direct && state.has_contact) {
    items.push_back(SessionMenuItem::ViewContact);
  }
  items.push_back(SessionMenuItem::ClearChat);
  // Closing a group thread is leaving the group; the close flow asks and hands ownership over when needed.
  items.push_back(state.kind == SessionKind::Group ? SessionMenuItem::LeaveGroup : SessionMenuItem::Delete);
  return items;
}

/** `muted_until` for "always" (same value as the store's `kThreadMutedForever`). */
inline constexpr int64_t kSessionMutedForever = std::numeric_limits<int64_t>::max();

enum class SessionMuteChoice { EightHours, OneWeek, Always };

inline int64_t MutedUntilFor(const SessionMuteChoice choice, const int64_t now_ms) {
  constexpr int64_t kHourMs = 60LL * 60 * 1000;
  switch (choice) {
  case SessionMuteChoice::EightHours:
    return now_ms + 8 * kHourMs;
  case SessionMuteChoice::OneWeek:
    return now_ms + 7 * 24 * kHourMs;
  case SessionMuteChoice::Always:
    return kSessionMutedForever;
  }
  return 0;
}

/** A mute is in force until its time passes; 0 is "not muted". */
inline bool ThreadIsMuted(const int64_t muted_until_ms, const int64_t now_ms) { return muted_until_ms > now_ms; }

/** Row order: pinned rows first, the most recently pinned on top; then newest activity first. */
inline bool SessionSortsBefore(const int64_t a_pinned_at, const int64_t a_updated_at, const int64_t b_pinned_at,
                               const int64_t b_updated_at) {
  if (a_pinned_at != b_pinned_at) {
    return a_pinned_at > b_pinned_at;
  }
  return a_updated_at > b_updated_at;
}

} // namespace pbr
