#pragma once

#include "lib/ui/NavBadgeFormat.h"
#include "lib/ui/ShellTypes.h"
#include "gui/shell/ShellNavigationPorts.h"

#include <functional>
#include <optional>

namespace pbr {

/** Unread totals for nav badges. Application computes from ConversationsHub + shell. */
struct BadgeUnreadInputs {
  int sessions_unread = 0;
  /** Reserved for contacts-tab queues; fed from ContactsShellBridge when present. */
  int contacts_unread = 0;
  /** Me-tab attention (reachability nudge, etc.). */
  bool me_attention = false;
  /** The number for the app's icon; empty: leave the icon alone (messaging not initialized, e.g. still locked). */
  std::optional<int> app_icon_count;
};

/**
 * The icon number: the raw unread total of every chat (the open chat included, unlike the nav badge). Empty — the
 * icon is left alone — while messaging is not initialized (a locked profile must not wipe the number the last run
 * left) or when the total could not be read (`unread_total` empty: 0 would look like "all read").
 * `count_muted`: whether the unread of muted chats (`muted_unread`, from ThreadIsMuted over each chat's local
 * prefs) is in the number. The owner has not decided; true is today's behaviour.
 */
inline std::optional<int> AppIconBadgeCount(bool messaging_initialized, std::optional<int> unread_total,
                                            int muted_unread, bool count_muted) {
  if (!messaging_initialized || !unread_total) {
    return std::nullopt;
  }
  const int shown = count_muted ? *unread_total : *unread_total - muted_unread;
  return shown > 0 ? shown : 0;
}

/** What BadgeAggregator's source reads from the app, gathered so that the arithmetic is testable. */
struct BadgeSourceFacts {
  bool messaging_initialized = false;
  /** Empty: the chats could not be listed. */
  std::optional<int> unread_total;
  /** Unread inside muted chats (0 until the muted question is decided; see AppIconBadgeCount). */
  int muted_unread = 0;
  bool count_muted_in_icon = true;
  /** The Chats tab is showing: the open chat's unread is read there, so the nav badge does not count it. */
  bool sessions_tab_active = false;
  int open_chat_unread = 0;
  int contacts_unread = 0;
  bool me_attention = false;
};

/** The nav badges deduct the open chat on the Chats tab; the icon number never does. */
BadgeUnreadInputs ComputeBadgeInputs(const BadgeSourceFacts& facts);

class BadgeAggregator {
public:
  BadgeAggregator() = default;

  /** App fills from ConversationsHub / ShellHost. Not a process singleton. Clear with BindSource({}). */
  void BindSource(std::function<BadgeUnreadInputs()> source);
  /** Nav badge write port. Clear via BindShellNavigation({}). */
  void BindShellNavigation(ShellNavigationPorts ports);
  /** The app icon number port (PlatformAppBadge). Clear via BindAppBadge({}). */
  void BindAppBadge(std::function<void(int)> set_app_badge);

  /** The next Refresh() sends the icon number again even if it is the one sent last (permission just granted, a push set its own). */
  void InvalidateAppBadge() { last_sent_.reset(); }
  /** Messaging stopped: the icon shows 0 now, whatever the next Refresh() finds. */
  void ClearAppBadge();

  void Refresh();
  const NavBadgeState& State() const { return state_; }

private:
  NavBadgeState state_;
  std::function<BadgeUnreadInputs()> source_;
  ShellNavigationPorts shell_navigation_;
  std::function<void(int)> set_app_badge_;
  /** The icon number last sent; empty: none sent yet (the first number with a value is always sent, 0 too). */
  std::optional<int> last_sent_;
};

} // namespace pbr
