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

/** One row in the in-call participant roster strip. */
struct CallRosterParticipantState {
  Rml::String name;
  bool audio_muted = false;
  bool video_enabled = false;
  bool is_local = false;
};

/** In-call chrome presentation mode (V031). */
enum class CallChromeMode {
  Expanded = 0,
  Minimized = 1,
  Immersive = 2,
};

inline const char* CallChromeModeName(CallChromeMode mode) {
  switch (mode) {
  case CallChromeMode::Minimized:
    return "minimized";
  case CallChromeMode::Immersive:
    return "immersive";
  case CallChromeMode::Expanded:
  default:
    return "expanded";
  }
}

/** Active in-call chrome. */
struct CallInProgressState {
  bool active = false;
  bool muted = false;
  bool camera_on = false;
  /** Speakerphone on (true) vs earpiece (false). UI: same icon, contrast encodes status. */
  bool speaker_on = false;
  bool stage_visible = false;
  bool remote_video = false;
  bool local_preview = false;
  /** True when joined count > 2 or call started from a group thread. */
  bool show_roster = false;
  /** Mid-call invite affordance. */
  bool show_invite = false;
  /** Show Retry after 1:1 P2P connect fail/timeout. */
  bool show_retry = false;
  /** Phone-like devices: earpiece / speakerphone toggle. */
  bool show_speaker = false;
  /** Expanded (top bar), Immersive (people grid), or Minimized (corner chip). */
  CallChromeMode mode = CallChromeMode::Expanded;
  /**
   * Minimized chip corner: 0 top-right, 1 top-left, 2 bottom-right, 3 bottom-left.
   * Applied when mode == Minimized.
   */
  int minimized_corner = 0;
  int participant_count = 0;
  std::vector<CallRosterParticipantState> roster;
  Rml::String call_id;
  Rml::String title;
  Rml::String subtitle;
  /** Secondary tip under subtitle (Local Network / mic / firewall). */
  Rml::String status_hint;
  Rml::String elapsed;
  Rml::String peer_label;
  /** Quantized mic level 0..5 for speaking meter bars. */
  int mic_level = 0;
  /** Quantized remote audio level 0..5. */
  int peer_level = 0;
  Rml::String mic_hint;
  Rml::String peer_hint;
  Rml::String remote_placeholder;
  /** Bound mode name for data-model / tests (`expanded` / `minimized` / `immersive`). */
  Rml::String mode_str = "expanded";
  /**
   * Path quality bars 0..4 (statusbar reach language). Label omitted when good
   * (`quality_label` empty); Fair/Poor/NoAudio set a short status string.
   */
  int quality_bars = 4;
  bool quality_ok = true;
  bool quality_warn = false;
  bool quality_error = false;
  Rml::String quality_label;
  Rml::String quality_hint;
  /** Debug-only subtitle under elapsed (`SFU · 24k · …`). */
  bool show_debug_subtitle = false;
  Rml::String debug_subtitle;
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
   * Left cluster: Brief · Direct · [divider · Help · Inbound] (+ sparse label). Right: activity.
   * See projects/network-status-chrome/.
   */
  bool statusbar_visible = false;
  bool statusbar_brief_visible = false;
  bool statusbar_brief_ok = false;
  bool statusbar_brief_failed = false;
  bool statusbar_brief_unknown = false;
  bool statusbar_direct_visible = false;
  bool statusbar_direct_ok = false;
  bool statusbar_direct_off = false;
  bool statusbar_direct_error = false;
  bool statusbar_direct_checking = false;
  bool statusbar_help_visible = false;
  bool statusbar_inbound_visible = false;
  bool statusbar_inbound_ok = false;
  bool statusbar_inbound_off = false;
  bool statusbar_load_circuit_visible = false;
  bool statusbar_load_media_visible = false;
  Rml::String statusbar_load_circuit_label;
  Rml::String statusbar_load_media_label;
  Rml::String statusbar_load_circuit_title;
  Rml::String statusbar_load_media_title;
  /** Sparse word for off/degraded states; empty when healthy. */
  Rml::String statusbar_label;
  bool statusbar_label_warn = false;
  bool statusbar_label_error = false;
  Rml::String statusbar_activity;
  /** Accessible names for icon-only slots (data-attr-title). */
  Rml::String statusbar_brief_title;
  Rml::String statusbar_direct_title;
  Rml::String statusbar_help_title;
  Rml::String statusbar_inbound_title;
  Rml::String statusbar_cluster_title;
  /** Hybrid status popover (network-status-chrome s2). */
  bool statusbar_popover_open = false;
  Rml::String statusbar_popover_brief_label;
  Rml::String statusbar_popover_direct_label;
  Rml::String statusbar_popover_reach_label;
  Rml::String statusbar_popover_reach_summary;
  bool statusbar_popover_help_visible = false;
  Rml::String statusbar_popover_help_label;
  bool statusbar_popover_upnp_visible = false;
  Rml::String statusbar_popover_upnp_label;
  bool statusbar_popover_error_visible = false;
  Rml::String statusbar_popover_error;
  bool statusbar_popover_load_visible = false;
  Rml::String statusbar_popover_circuit_load;
  Rml::String statusbar_popover_media_sessions;
  Rml::String statusbar_popover_media_participants;

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
