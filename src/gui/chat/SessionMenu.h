#pragma once

#include <vector>

namespace pbr {

/** Pure rules for the context menu of a row in the sessions list (no RmlUi, no storage). */

enum class SessionKind { Ai, Direct, Group };

enum class SessionMenuItem { MarkUnread, MarkRead, ViewContact, ClearChat, Delete, LeaveGroup };

struct SessionMenuState {
  SessionKind kind = SessionKind::Ai;
  /** The row shows an unread badge. */
  bool unread = false;
  /** Direct chat whose peer is a saved contact. */
  bool has_contact = false;
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
  if (state.kind == SessionKind::Direct && state.has_contact) {
    items.push_back(SessionMenuItem::ViewContact);
  }
  items.push_back(SessionMenuItem::ClearChat);
  // Closing a group thread is leaving the group; the close flow asks and hands ownership over when needed.
  items.push_back(state.kind == SessionKind::Group ? SessionMenuItem::LeaveGroup : SessionMenuItem::Delete);
  return items;
}

} // namespace pbr
