#pragma once

#include <RmlUi/Core/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {

enum class PaneRole { Primary, Secondary, Auxiliary, Transient };

enum class LayoutMode { Compact, Expanded };

enum class NavTab { Home, Sessions, Contacts, Me };

enum class OverlayKind { Generic, Alert, Confirm, Custom };

enum class InterruptionKind {
  None,
  CompactChatOverlay,
  AuxiliarySheet,
  AccountSheet,
  Transient,
  OverlayLayer,
  Dialog,
  CallRing,
  CallInProgress,
  PinGate,
};

/** At most one compact chrome bar uses backdrop frost per frame. */
enum class CompactChromeFrostSurface {
  None,
  BottomNav,
  ChatOverlayHeader,
  AuxiliarySheetChrome,
  AccountSheetHeader,
  TransientHeader,
};

enum class ToastDuration { Short, Long };

struct ShellConfig {
  float compact_breakpoint_dp = 768.f;
  float nav_rail_width_dp = 72.f;
  float secondary_width_dp = 240.f;
  float auxiliary_width_dp = 320.f;
  float toolbar_height_dp = 48.f;
  /** Desktop custom title bar height (0 on mobile). */
  float titlebar_height_dp = 36.f;
  /** Desktop + expanded bottom status bar height. */
  float statusbar_height_dp = 24.f;
  /**
   * Window control cluster width for hit-test exclusion.
   * Win/Linux icon strip ~120dp; macOS traffic lights ~68dp (set in ShellHost).
   */
  float titlebar_controls_width_dp = 120.f;
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
  /**
   * Body top offset — kept at 0 so the document background paints under the
   * translucent status bar (edge-to-edge blend).
   */
  float shell_top_dp = 0.f;
  /** Bottom inset for home indicator / IME; applied to the shell document body. */
  float shell_bottom_dp = 0.f;
  /**
   * Top inset applied to #shell-root / chrome so content clears the status bar
   * (mobile) and/or the desktop custom title bar.
   */
  float content_top_dp = 0.f;
  /** Extra padding inside content so the last row clears the floating nav. */
  float content_padding_bottom_dp = 0.f;
  float chrome_bottom_dp = 0.f;
  /** Auxiliary sheet offset above the compact nav rail (account sheet uses chrome_bottom_dp). */
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
  /** When non-empty, used as the primary button label instead of common.ok. */
  Rml::String ok_label;
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

/** Incoming call ring. */
struct CallRingState {
  bool active = false;
  bool pulse = false;
  /** True when Accept would end an existing local call. */
  bool conflict = false;
  Rml::String call_id;
  Rml::String caller_label;
  Rml::String media_label;
  Rml::String eyebrow;
  Rml::String conflict_hint;
  Rml::String accept_label;
  Rml::String decline_label;
};

/** Active in-call chrome. */
struct CallInProgressState {
  bool active = false;
  bool muted = false;
  bool camera_on = false;
  bool stage_visible = false;
  bool remote_video = false;
  bool local_preview = false;
  Rml::String call_id;
  Rml::String title;
  Rml::String subtitle;
  Rml::String elapsed;
  Rml::String peer_label;
  /** Quantized mic level 0..5 for speaking meter bars. */
  int mic_level = 0;
  /** Quantized remote audio level 0..5. */
  int peer_level = 0;
  Rml::String mic_hint;
  Rml::String peer_hint;
  Rml::String remote_placeholder;
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
  /** Reserved for contacts-tab queues (intro requests, pending invites). Not chat unread; always 0 until those exist. */
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
  CallRingState call_ring;
  CallInProgressState call_in_progress;

  bool activity_visible = false;

  /**
   * Desktop + expanded bottom status bar (read-only ambient chrome).
   * Hidden on compact layout and on mobile/tablet platforms.
   */
  bool statusbar_visible = false;
  Rml::String statusbar_connection;
  Rml::String statusbar_activity;

  NavBadgeState nav_badges;

  float shell_width_dp = 1280.f;
  /** Top safe-area inset in dp (SDL and/or machine prefs); clears status bar / notch. */
  int safe_area_top_dp = 0;
  /** Bottom safe-area inset in dp (SDL and/or machine prefs). */
  int safe_area_bottom_dp = 0;

  /** From profile prefs — opaque compact chrome only (no backdrop frost). */
  bool reduce_transparency = false;
  /** From profile prefs — when false, frost tier disabled; layout unchanged. */
  bool compact_chrome_frost = true;

  /** Desktop custom title bar (borderless window chrome). */
  bool titlebar_visible = false;
  /** macOS: drawn traffic lights on the leading edge. */
  bool titlebar_traffic_lights = false;
  bool window_maximized = false;

  /**
   * CJK/emoji fallback faces ready for chrome text. False until deferred load when the
   * UI language needs CJK; true immediately for Latin-only UI.
   */
  bool fonts_ready = true;
  /** Silent / deferred vault unlock in progress after first present. */
  bool unlock_in_progress = false;
};

} // namespace pbr
