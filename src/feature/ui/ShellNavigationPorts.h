#pragma once

#include "base/ui/ShellTypes.h"

#include <functional>
#include <string>

namespace pbr {

/** Read-only shell chrome fields settings (and other surfaces) need without ShellHost. */
struct ShellChromeSnapshot {
  LayoutMode layout_mode = LayoutMode::Expanded;
  NavTab nav_tab = NavTab::Home;
  bool account_sheet_open = false;
  /** Top transient pane is `settings_detail`. */
  bool settings_detail_transient = false;
  /** Expanded Me primary pane shows settings detail. */
  bool settings_detail_primary = false;
  /** Top transient pane is `contact_detail`. */
  bool contact_detail_transient = false;
  /** Expanded Contacts primary pane shows contact detail. */
  bool contact_detail_primary = false;
  std::string primary_pane_key;
  NavBadgeState nav_badges{};
  std::string banner_message;
  bool auxiliary_open = false;
};

inline ShellChromeSnapshot ProjectShellChromeSnapshot(const ShellState& state) {
  ShellChromeSnapshot snap;
  snap.layout_mode = state.layout_mode;
  snap.nav_tab = state.nav_tab;
  snap.account_sheet_open = state.account_sheet_open;
  snap.settings_detail_transient =
      !state.transient_stack.empty() && state.transient_stack.back().spec.key == "settings_detail";
  snap.settings_detail_primary = state.primary_pane_key == "settings_detail";
  snap.contact_detail_transient =
      !state.transient_stack.empty() && state.transient_stack.back().spec.key == "contact_detail";
  snap.contact_detail_primary = state.primary_pane_key == "contact_detail";
  snap.primary_pane_key = state.primary_pane_key.c_str();
  snap.nav_badges = state.nav_badges;
  snap.banner_message = state.banner_message.c_str();
  snap.auxiliary_open = state.auxiliary_open;
  return snap;
}

/**
 * Shell navigation / layout ports for settings and other UI surfaces.
 * Declared here (consumer); Application fills from ShellHost. Not a singleton.
 */
struct ShellNavigationPorts {
  std::function<ShellChromeSnapshot()> snapshot;

  std::function<void(const std::string& id)> clear_local_back;
  std::function<void(const std::string& id, std::function<void()> commit)> push_local_back;
  std::function<bool(const std::string& id)> has_local_back;

  std::function<void(NavTab tab)> select_nav_tab;
  std::function<void()> open_account_sheet;
  std::function<void()> close_account_sheet;
  std::function<void()> clear_primary_pane;
  std::function<void(const std::string& key)> set_primary_pane;
  std::function<void(const PaneSpec& spec)> push_transient;
  std::function<void()> pop_transient;
  std::function<void()> open_compact_chat;
  std::function<void()> close_compact_chat;
  /** Instant dismiss (Escape / local-back commit). Returns true when consumed. */
  std::function<bool()> request_dismiss_instant;
  std::function<void()> refresh_dismiss_gestures;
  std::function<void()> request_remount_nav_rail;
  std::function<void(bool visible, const Rml::String& message)> set_activity;
  std::function<void()> dirty_window;
  std::function<void(bool restore_focus_after, const char* reason)> request_sync_layout;
  std::function<void(const NavBadgeState& badges)> set_nav_badges;
  std::function<void(bool available)> set_auxiliary_available;
  std::function<void()> open_auxiliary;
  std::function<void()> close_auxiliary;
  std::function<int(const PaneSpec& spec)> push_layer;
  std::function<void(int layer_id)> close_layer;
};

class ShellHost;

/** App composition helper — fills all navigation ports from ShellHost. */
ShellNavigationPorts MakeShellNavigationPorts(ShellHost& shell);

} // namespace pbr
