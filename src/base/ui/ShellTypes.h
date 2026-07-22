#pragma once

#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {

enum class PaneRole { Primary, Secondary, Auxiliary, Transient };

enum class LayoutMode { Compact, Expanded };

enum class NavTab { Home, Sessions, Contacts };

enum class OverlayKind { Generic, Alert, Confirm, Custom };

enum class InterruptionKind {
  None,
  CompactChatOverlay,
  AuxiliarySheet,
  AccountSheet,
  Transient,
  OverlayLayer,
  Dialog,
  PinGate,
};

enum class ToastDuration { Short, Long };

struct ShellConfig {
  float compact_breakpoint_dp = 768.f;
  float nav_rail_width_dp = 72.f;
  float secondary_width_dp = 240.f;
  float auxiliary_width_dp = 320.f;
  float toolbar_height_dp = 48.f;
  float frame_padding_dp = 24.f;
  /** Compact bottom nav bar height (excludes safe-area inset). */
  float compact_nav_height_dp = 56.f;
  /** Horizontal inset for floating compact nav pill (lg1 layout). */
  float compact_nav_horizontal_inset_dp = 12.f;
  size_t max_toasts = 3;
  float toast_short_ms = 3000.f;
};

/** Derived dp offsets for compact floating chrome + safe-area (lg1). */
struct CompactChromeLayout {
  /** Top inset for status bar / notch; applied to the shell document body. */
  float shell_top_dp = 0.f;
  /** Bottom inset for home indicator / IME; applied to the shell document body. */
  float shell_bottom_dp = 0.f;
  /** Extra padding inside content so the last row clears the floating nav. */
  float content_padding_bottom_dp = 0.f;
  float chrome_bottom_dp = 0.f;
  float sheet_bottom_dp = 0.f;
  float chrome_horizontal_inset_dp = 0.f;
};

struct PaneSpec {
  std::string key;
  std::string rml_path;
  PaneRole role = PaneRole::Primary;
  Rml::String toolbar_label;
  bool provides_composer = false;
};

struct PaneState {
  PaneSpec spec;
  int id = 0;
};

struct OverlayEntry {
  int id = 0;
  OverlayKind kind = OverlayKind::Generic;
  std::string rml_path;
  std::function<void(bool)> on_result;
};

struct ToastEntry {
  int id = 0;
  Rml::String message;
  float expires_at_ms = 0.f;
};

struct DialogState {
  bool active = false;
  OverlayKind kind = OverlayKind::Alert;
  Rml::String title;
  Rml::String message;
  bool show_cancel = false;
  bool show_checkbox = false;
  Rml::String checkbox_label;
  bool checkbox_checked = false;
  bool show_prompt = false;
  Rml::String prompt_value;
  std::function<void(bool confirmed, bool checkbox_checked)> on_result;
  std::function<void(bool confirmed, std::string prompt_value)> on_prompt_result;
};

/** PIN unlock / create / chooser overlay. Unlock is mandatory; create and chooser may cancel. */
struct PinGateState {
  bool active = false;
  bool chooser_mode = false;
  bool create_mode = false;
  Rml::String title;
  Rml::String message;
  Rml::String error;
  Rml::String pin;
  Rml::String pin_confirm;
  std::function<void(bool unlocked)> on_result;
};

struct PaneVisibility {
  bool secondary = false;
  bool primary = true;
  bool auxiliary = false;
  bool compact_nav_page = false;
  bool compact_chat_overlay = false;
  bool auxiliary_sheet = false;
};

struct NavBadgeState {
  int sessions_unread = 0;
  int contacts_unread = 0;
  bool me_attention = false;
  Rml::String sessions_unread_display;
  Rml::String contacts_unread_display;
};

struct ShellState {
  LayoutMode layout_mode = LayoutMode::Expanded;
  Rml::String layout_mode_str = "expanded";
  NavTab nav_tab = NavTab::Home;
  Rml::String nav_tab_str = "home";
  std::vector<PaneState> panes;
  std::vector<PaneState> transient_stack;
  std::vector<OverlayEntry> overlay_stack;

  bool compact_chat_open = false;
  bool account_sheet_open = false;
  bool auxiliary_open = false;
  bool auxiliary_available = false;
  bool transient_active = false;

  Rml::String primary_pane_key;

  Rml::String banner_message;
  std::vector<ToastEntry> toasts;
  DialogState dialog;
  PinGateState pin_gate;

  bool activity_visible = false;

  NavBadgeState nav_badges;

  float shell_width_dp = 1280.f;
  /** Top safe-area inset in dp (SDL and/or machine prefs); clears status bar / notch. */
  int safe_area_top_dp = 0;
  /** Bottom safe-area inset in dp (SDL and/or machine prefs). */
  int safe_area_bottom_dp = 0;
};

} // namespace pbr
