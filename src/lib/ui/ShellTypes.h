#pragma once

#include "lib/ui/CallChipDock.h"

#include <ui/base/Types.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {

enum class PaneRole { Primary, Secondary, Auxiliary, Transient };

enum class LayoutMode { Compact, Expanded };

enum class NavTab { Home, Sessions, Contacts, Me };

enum class OverlayKind { Generic, Alert, Confirm, Custom };

/**
 * Where a phone page sheet stops (Kenneth 2026-11-04, every sheet alike): Large is the height it opens at (about 4/5
 * of the screen, a strip of the page behind left in sight); Full near the top of the screen; Half about half of it;
 * Small about a third (the title row and the top of the content).
 */
enum class SheetDetent { Full, Large, Half, Small };

enum class InterruptionKind {
  None,
  CompactChatOverlay,
  AuxiliarySheet,
  Transient,
  OverlayLayer,
  Dialog,
  CallRing,
  LiveSessionOn,
  PinGate,
};

/** Which compact chrome is frosted: one surface at a time (BottomNav: the tab page's nav bar and its tab bar). */
enum class CompactChromeFrostSurface {
  None,
  BottomNav,
  ChatOverlayHeader,
  AuxiliarySheetChrome,
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
  /** Compact bottom nav bar height (excludes safe-area inset): the two-word tab bar (DEV/PLAN-minimal-nav.md A). */
  float compact_nav_height_dp = 48.f;
  /** Horizontal inset for the compact tab bar (full-width, so 0). */
  float compact_nav_horizontal_inset_dp = 0.f;
  size_t max_toasts = 3;
  float toast_short_ms = 3000.f;
};

/** What paints along the bottom edge of the compact shell (decides who covers the home-indicator strip). */
enum class CompactBottomSurface {
  None,     ///< Desktop / nothing: the document body shrinks by the inset.
  TabBar,   ///< Tab bar shown: the bar grows by the inset and paints the strip.
  Composer, ///< Chat overlay (no tab bar): the composer mount pads by the inset and paints the strip.
  Page,     ///< Transient page: the page pads by the inset and paints the strip.
};

/** Derived dp offsets for compact floating chrome + safe-area (lg1). */
struct CompactChromeLayout {
  /**
   * Body top offset — kept at 0 so the document background paints under the
   * translucent status bar (edge-to-edge blend).
   */
  float shell_top_dp = 0.f;
  /** Bottom inset for the home indicator; applied to the shell document body. 0 while the keyboard is up, so the body
   *  (theme background) stays full-bleed and paints behind the keyboard — the keyboard inset lifts the content instead
   *  (content_bottom_inset_dp), the same edge-to-edge trick the top uses (content_top_dp under the status bar).
   *  Without that the window's clear colour (transparent black) shows through the keyboard's rounded top corners. */
  float shell_bottom_dp = 0.f;
  /** Keyboard inset applied to #shell-root (not the body) so content clears the on-screen keyboard while the body
   *  background still fills behind it. 0 when no keyboard is up. */
  float content_bottom_inset_dp = 0.f;
  /**
   * Top inset applied to #shell-root / chrome so content clears the status bar
   * (mobile) and/or the desktop custom title bar.
   */
  float content_top_dp = 0.f;
  /** Extra padding inside content so the last row clears the floating nav. */
  float content_padding_bottom_dp = 0.f;
  /** The on-screen keyboard is up over a tab-bar page: the tab bar is hidden (the keyboard covers it) so content is
   *  not squeezed above a floating bar. */
  bool tab_bar_hidden = false;
  float chrome_bottom_dp = 0.f;
  /**
   * Home-indicator inset the bottom surface (tab bar / composer mount / transient page) pads itself by
   * so its background reaches the screen edge. 0 while the keyboard is up or on desktop.
   */
  float bottom_surface_padding_dp = 0.f;
  /** Auxiliary sheet offset above the compact nav rail. */
  float sheet_bottom_dp = 0.f;
  float chrome_horizontal_inset_dp = 0.f;
  /**
   * Phone toast: distance of the toast stack from the bottom of the document body, so the pill sits just
   * above the tab bar / composer (and so above the keyboard). 0 when there is no bottom surface (desktop).
   */
  float toast_bottom_dp = 0.f;
};

struct PaneSpec {
  std::string key;
  std::string rml_path;
  PaneRole role = PaneRole::Primary;
  ui::String toolbar_label;
  bool provides_composer = false;
  /**
   * When non-empty, PushLayer stores this element id for RestoreFocus on close
   * instead of the live focus element (often the button that opened the overlay).
   */
  std::string return_focus_id;
  /**
   * PushLayer on phones: an iOS page sheet (rises from the bottom to SheetDetent::Large, rounded top, grab handle,
   * the page behind dimmed; dragged between small, half, large and full, let go low or flicked down from small to
   * close) instead of the centred
   * panel. Desktop is unchanged.
   */
  bool page_sheet = false;
  /**
   * PushLayer on phones: a full-screen page reaching every screen edge (under the status bar and the home indicator)
   * with no scrim, frame or close button: the view brings its own way out and keeps its controls inside the
   * shell-overlay-body, which ShellHost pads by the safe areas. Desktop is unchanged.
   */
  bool full_screen = false;
  /** A phone page sheet's first stop (the reaction emoji picker opens at Small, about a third of the screen). */
  SheetDetent sheet_detent = SheetDetent::Large;
};

/** IME-replacement bottom panel (mobile/compact); not a modal overlay. */
struct BottomChromeSpec {
  std::string key;
  std::string rml_path;
  /** > 0: the panel is this share of the window's height (a sheet, Kenneth 2026-11-05: about a third) and keeps it;
   *  0: it takes the on-screen keyboard's height, like the keyboard it stands in for. */
  float height_share = 0.f;
};

struct PaneState {
  PaneSpec spec;
  int id = 0;
};

struct OverlayEntry {
  int id = 0;
  OverlayKind kind = OverlayKind::Generic;
  /** ViewCatalog key (e.g. emoji_picker); used for presentation variants. */
  std::string key;
  std::string rml_path;
  std::function<void(bool)> on_result;
  /** PaneSpec::page_sheet: shown as a page sheet on phones. */
  bool page_sheet = false;
  /** Where the sheet stands now (moved by its drag); while the keyboard is up it shows full height anyway. */
  SheetDetent sheet_detent = SheetDetent::Large;
  /** PaneSpec::full_screen: edge to edge on phones. */
  bool full_screen = false;
};

struct ToastEntry {
  int id = 0;
  ui::String message;
  float expires_at_ms = 0.f;
};

struct DialogState {
  bool active = false;
  OverlayKind kind = OverlayKind::Alert;
  ui::String title;
  ui::String message;
  /** When non-empty, used as the primary button label instead of common.ok. */
  ui::String ok_label;
  bool show_cancel = false;
  /** The confirm button deletes / removes / resets something: shown in the danger colour on the phone. */
  bool destructive = false;
  bool show_checkbox = false;
  ui::String checkbox_label;
  bool checkbox_checked = false;
  bool show_prompt = false;
  ui::String prompt_value;
  std::function<void(bool confirmed, bool checkbox_checked)> on_result;
  std::function<void(bool confirmed, std::string prompt_value)> on_prompt_result;
};

/** PIN unlock / create / chooser overlay. Unlock is mandatory; create and chooser may cancel. */
struct PinGateState {
  bool active = false;
  /** No vault yet: I'm new vs I already have an account. */
  bool identity_fork_mode = false;
  bool chooser_mode = false;
  bool create_mode = false;
  /** Paste `pp-browser-link-device-v1` after choosing a PIN on this device. */
  bool link_paste_mode = false;
  /** The paste page asks for a recovery phrase (multi-device M023) instead of a transfer code. */
  bool phrase_flow = false;
  /** On the phrase page: the saved copy picked to restore from as well (its file name; empty: none). */
  ui::String saved_copy_name;
  /** Plain unlock (vault exists): shows the "Forgot PIN?" link; `forgot_open` expands its explanation. */
  bool unlock_mode = false;
  bool forgot_open = false;
  /** Unlock with a call waiting on a locked phone (PIN grace window): "unlock your phone" + "Enter PIN instead". */
  bool waiting_device_unlock = false;
  /** Settings asks for the PIN once (transfer code, recovery phrase) / changes it (old → new → again), B2. */
  bool verify_mode = false;
  bool change_mode = false;
  /** Field mode: the "confirm" field is out too (a new PIN: create, change's new-PIN step). */
  bool ask_confirm = false;
  /**
   * The app's own number pad instead of a field (phones only; a known digit PIN, or a new PIN). DEV/PLAN-pin-gate-
   * redesign.md B. `pin` / `pin_confirm` are never bound to an input then: the markup has none, no system keyboard.
   */
  bool keypad = false;
  /** The PIN is being checked (Argon2): keys, backspace and the links do nothing; the dots pulse. */
  bool keypad_busy = false;
  /** Dots drawn (unlock: the PIN's length; a new PIN: digits typed so far; its repeat: the first entry's length). */
  int keypad_slots = 0;
  int keypad_filled = 0;
  /** The bottom-left "OK" key (a new PIN's first entry with 4 or more digits). */
  bool keypad_ok_shown = false;
  /** The link under the pad to the field ("Use the keyboard" / "Use letters and numbers"). */
  ui::String keypad_text_label;
  /** Field mode: "Use the number pad" is offered (phones; a known digit PIN, or a new PIN). */
  bool keypad_offered = false;
  /**
   * Unlock with face / fingerprint unlock on (at-rest A014): the pad's bottom-left key (no "OK" is due there) and,
   * in field mode, a link prompt again. `biometric_kind` is `face`, `touch` or `fingerprint` (icon and words).
   */
  bool biometric_key = false;
  ui::String biometric_kind;
  /** One line about face / fingerprint unlock on the door ("" none): pin.biometric_reset_hint after it was lost. */
  ui::String biometric_hint;
  /**
   * Face / fingerprint unlock on: the door opens with only the sensor's key and an "Enter PIN" button (as the
   * phone's own lock screen); the pad / field come after that button, or once the biometric unlock is lost.
   */
  bool biometric_first = false;
  ui::String title;
  ui::String message;
  ui::String error;
  ui::String pin;
  ui::String pin_confirm;
  /** Bound to the paste textarea's data-value. MUST stay small: data-if only sets display:none, so a hidden textarea
   *  still receives this value and shaping a multi-KB string freezes the app (B166/B167). An over-long paste is kept
   *  out of here (empty) and in `link_payload_full` instead. */
  ui::String link_payload;
  /** B166/B167: the real pasted value when it is too big to render; never bound to a textarea. Submit reads this when
   *  `payload_hidden`. A typed or short-pasted recovery phrase uses `link_payload` and leaves this empty. */
  ui::String link_payload_full;
  bool payload_hidden = false;
  std::function<void(bool unlocked)> on_result;
};

/** Incoming call ring. */
struct CallRingState {
  bool active = false;
  bool pulse = false;
  /** True when Accept would end an existing local call. */
  bool conflict = false;
  /** P001: initiation offer > 0 — show waive / take-all actions. */
  bool show_pricing = false;
  /** Take-all enabled only when settlement rails exist. */
  bool accept_charge_enabled = false;
  ui::String call_id;
  ui::String caller_label;
  ui::String media_label;
  /** True when initiator allowed video for this session (V035). */
  bool video_allowed = false;
  ui::String eyebrow;
  ui::String conflict_hint;
  /** V062 5: ringing in as a guest of a group's call: "Joining shows you to the N people in this call." ("": not). */
  ui::String guest_hint;
  ui::String accept_label;
  /** I2/M3: label for the video-ring "voice answer" button (free / end-and-accept wording). */
  ui::String voice_answer_label;
  ui::String decline_label;
  ui::String pricing_label;
  ui::String accept_charge_label;
  ui::String accept_charge_hint;
  /** Short label under the round accept/video circle button (WeChat-style ring redesign). */
  ui::String accept_short;
  /** Short label under the round voice-answer circle button. */
  ui::String voice_short;
  // Phone ring page (DESIGN-pp-ui 8.12, mockups 06-CallRing / 06-CallGroupRing): the caller (or the group) big, what
  // kind of call under it. Avatar fields are written into the markup (a change remounts).
  ui::String phone_title;
  ui::String phone_subtitle;
  bool group = false;
  bool has_avatar = false;
  ui::String avatar_src;
  ui::String avatar_letter = "?";
  int avatar_tone = 0;
};

/** One line of the phone call's "how this call got through" sheet (CallConnectStory events, in words). */
struct CallStoryLine {
  std::string time;
  std::string text;
  /** Drawn in the secure colour (how it connected, the encryption, a closer way); else success green. */
  bool secure = false;

  bool operator==(const CallStoryLine&) const = default;
};

/** One row in the in-call participant roster strip. */
struct CallRosterParticipantState {
  ui::String name;
  /** Decimal publisher stream_id for Immersive peer tiles. */
  ui::String stream_id;
  /** Communicating identity (not bound in RML). */
  ui::String identity;
  bool audio_muted = false;
  bool video_enabled = false;
  bool is_local = false;
  bool has_remote_video = false;
  bool has_avatar = false;
  ui::String avatar_src;
  ui::String avatar_letter = "?";
  int avatar_tone = 0;
  /** Speaking now (CallActiveSpeaker::Speaking; we by our microphone): the phone page rings the tile. Bound, and
   * part-dirtied on its own (CallChromeChangedVariables), at most every CallController::kSpeakingRingRefreshMs. */
  bool speaking = false;
  /** V060: the OS has their call audio (a phone call, Siri): the tile reads "On hold", dimmed. */
  bool on_hold = false;
  /** V060: their camera is off because their app is in the background: the tile reads "Camera paused". */
  bool video_paused = false;
  /** V062: a guest in the group's call: the tile reads "Guest". */
  bool guest = false;
};

/**
 * V062: a guest of the group's call this device may do something about, as the call chrome lists them (written into
 * the markup: a change remounts). `identity` is an account id (markup-safe).
 */
struct CallGuestActionState {
  ui::String identity;
  /** "Erin · Bob's guest". */
  ui::String line;
  /** "Add Erin to the group" (empty: not offered). */
  ui::String add_to_group_label;
  bool can_remove = false;

  bool operator==(const CallGuestActionState&) const = default;
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
  /** V060: this call is on hold, here or on the other side (the 1:1 page shows an "On hold" pill; the line says whose). */
  bool on_hold = false;
  bool muted = false;
  bool camera_on = false;
  /** The camera on is the front one: the flip button is lit (#83). */
  bool camera_front = false;
  /** Speakerphone on (true) vs earpiece (false). UI: same icon, contrast encodes status. */
  bool speaker_on = false;
  bool stage_visible = false;
  bool remote_video = false;
  bool local_preview = false;
  /** True when joined count > 2 or call started from a group thread. */
  bool show_roster = false;
  /** Mid-call invite affordance. */
  bool show_invite = false;
  /** G022: "Make a group…" (a call with no group and 3+ in it). */
  bool show_make_group = false;
  /** G022: the banner of a group someone here made with everyone ("" = none); Join / Not now under it. */
  ui::String group_invite_text;
  /** V062: the guests this device may remove or invite to the group (owner / admins; whoever may invite). */
  std::vector<CallGuestActionState> guest_actions;
  /** Show Retry after 1:1 P2P connect fail/timeout. */
  bool show_retry = false;
  /** An outgoing call still ringing: the phone shows a labelled Cancel (ShowsCancelCall). */
  bool calling = false;
  /** Phone-like devices: earpiece / speakerphone toggle. */
  bool show_speaker = false;
  /** Hide Camera when video disallowed, encoder missing, or hop A↑ too small for video_lo. */
  bool show_camera = true;
  /** Phones with the camera on: switch front / rear camera. */
  bool show_flip = false;
  /** Expanded (top bar), Immersive (people grid), or Minimized (corner chip). */
  CallChromeMode mode = CallChromeMode::Expanded;
  /**
   * #166: where the minimised chip rests after a drag (CallChipDock): 0 never dragged this call (the default top-right
   * corner class), 1 left side, 2 right side; and its height in the band, 0 (top) .. 1000 (bottom), ignored for 0.
   * Applied when mode == Minimized; reset with each call (CallController).
   */
  int minimized_side = 0;
  int minimized_y_permille = 0;
  int participant_count = 0;
  std::vector<CallRosterParticipantState> roster;
  /** Immersive's spotlit roster row (its stream_id; empty: the grid) — V059, CallSpotlight.h. */
  ui::String spotlight_stream;
  /** The spotlit tile covers the area, cropped (else the whole frame, letterboxed). Double-tap toggles. */
  bool spotlight_fill = true;
  /** With a spotlight: its row, and everyone else's for the strip (copies of roster rows). */
  std::vector<CallRosterParticipantState> spotlight_rows;
  std::vector<CallRosterParticipantState> strip_rows;
  ui::String call_id;
  ui::String title;
  ui::String subtitle;
  /** Secondary tip under subtitle (Local Network / mic / firewall). */
  ui::String status_hint;
  ui::String elapsed;
  ui::String peer_label;
  /** Quantized mic level 0..5 for speaking meter bars. */
  int mic_level = 0;
  /** Quantized remote audio level 0..5. */
  int peer_level = 0;
  ui::String mic_hint;
  ui::String peer_hint;
  ui::String remote_placeholder;
  /** Bound mode name for data-model / tests (`expanded` / `minimized` / `immersive`). */
  ui::String mode_str = "expanded";
  /**
   * Path quality bars 0..4 (statusbar reach language). Label omitted when good
   * (`quality_label` empty); Fair/Poor/NoAudio set a short status string.
   */
  int quality_bars = 4;
  bool quality_ok = true;
  bool quality_warn = false;
  bool quality_error = false;
  ui::String quality_label;
  ui::String quality_hint;
  /** Debug-only subtitle under elapsed (`SFU · 24k · …`). */
  bool show_debug_subtitle = false;
  ui::String debug_subtitle;

  // --- Phone call page (DESIGN-pp-ui 8.12 with its 2026-11-04 connection story; mockups 06-Call*, 08). Fields
  // written into the markup remount when they change (CallChromeSync); the bound ones dirty only themselves.
  /** The other person's name (1:1) or the group's name. */
  ui::String phone_title;
  bool video = false;
  bool peer_has_avatar = false;
  ui::String peer_avatar_src;
  ui::String peer_avatar_letter = "?";
  int peer_avatar_tone = 0;
  bool self_has_avatar = false;
  ui::String self_avatar_src;
  ui::String self_avatar_letter = "?";
  int self_avatar_tone = 0;
  /** Couldn't connect / disconnected: the failure page (plain words, Try again / Close). */
  bool phone_failed = false;
  ui::String failed_text;
  /** Before connect: the moving line, the steps (0: calling, 1: answered) and the rotating tips (RCSS only). */
  bool story_active = false;
  int story_stage = 0;
  ui::String story_step1;
  ui::String story_step2;
  ui::String story_step3;
  /** Just connected (1:1): "Connected directly, 0.9 s" for 4 s, then it fades (bound). */
  ui::String result_text;
  bool result_fading = false;
  /** In call: PP's own signal bar, two dots joined by a line and four bars (bound; changes at most every 500 ms). */
  bool signal_shown = false;
  int signal_bars = 4;
  /** 0 good (secure), 1 fair (warn + word), 2 poor (danger + word). */
  int signal_tone = 0;
  ui::String signal_word;
  /** The top pill (DESIGN-pp-ui 8.11): slower line, reconnecting, closer way (bound). */
  ui::String notice_text;
  bool notice_warn = false;
  bool notice_fading = false;
  /** The reconnect capsule above the button bar (#171; empty text: hidden). Bound. */
  ui::String reconnect_text;
  bool reconnect_ok = false;
  bool reconnect_fading = false;
  /** The call timer greys while the line is down. */
  bool reconnect_dim = false;
  /** The "how this call got through" sheet (a tap on the signal bar). */
  bool story_sheet_open = false;
  std::vector<CallStoryLine> story_lines;
  ui::String story_how;
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
  ui::String sessions_unread_display;
  ui::String contacts_unread_display;
};

struct ShellState {
  LayoutMode layout_mode = LayoutMode::Expanded;
  ui::String layout_mode_str = "expanded";
  NavTab nav_tab = NavTab::Home;
  ui::String nav_tab_str = "home";
  std::vector<PaneState> panes;
  std::vector<PaneState> transient_stack;
  std::vector<OverlayEntry> overlay_stack;

  bool compact_chat_open = false;
  /** Phone Home: the AI conversation list slides in from the left (open_ai_drawer / close_ai_drawer). */
  bool ai_drawer_open = false;
  bool auxiliary_open = false;
  bool auxiliary_available = false;
  bool transient_active = false;

  /** Expanded layout only: secondary pane width (dp, clamped by ShellLayout) and user collapse. */
  int sidebar_width_dp = 240;
  bool sidebar_collapsed = false;
  /** Phone layout: the active root page (Chats / Contacts / Me) has scrolled its 52dp large title away. */
  bool large_title_collapsed = false;
  /** Expanded layout only: auxiliary pane width (dp, clamped by ShellLayout). */
  int auxiliary_width_dp = 320;

  ui::String primary_pane_key;

  ui::String banner_message;
  /** Banners go away on their own (ShellFeedback::kBannerTtlMs); 0 = none showing. */
  float banner_expires_at_ms = 0.f;
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
  ui::String statusbar_load_circuit_label;
  ui::String statusbar_load_media_label;
  ui::String statusbar_load_circuit_title;
  ui::String statusbar_load_media_title;
  /** Sparse word for off/degraded states; empty when healthy. */
  ui::String statusbar_label;
  bool statusbar_label_warn = false;
  bool statusbar_label_error = false;
  /** Accessible names for icon-only slots (data-attr-title). */
  ui::String statusbar_brief_title;
  ui::String statusbar_direct_title;
  ui::String statusbar_help_title;
  ui::String statusbar_inbound_title;
  ui::String statusbar_cluster_title;
  /** Hybrid status popover (network-status-chrome s2). */
  bool statusbar_popover_open = false;
  ui::String statusbar_popover_brief_label;
  ui::String statusbar_popover_direct_label;
  ui::String statusbar_popover_reach_label;
  ui::String statusbar_popover_reach_summary;
  bool statusbar_popover_help_visible = false;
  ui::String statusbar_popover_help_label;
  bool statusbar_popover_upnp_visible = false;
  ui::String statusbar_popover_upnp_label;
  bool statusbar_popover_error_visible = false;
  ui::String statusbar_popover_error;
  bool statusbar_popover_load_visible = false;
  ui::String statusbar_popover_circuit_load;
  ui::String statusbar_popover_media_sessions;
  ui::String statusbar_popover_media_participants;
  bool statusbar_popover_network_visible = false;
  ui::String statusbar_popover_network_links;
  ui::String statusbar_popover_network_parked;
  ui::String statusbar_popover_network_rate;
  ui::String statusbar_popover_network_rtt;
  ui::String statusbar_popover_network_resend;
  ui::String statusbar_popover_relay_rate;

  NavBadgeState nav_badges;

  float shell_width_dp = 1280.f;
  /**
   * #166: the dropped call chip's last top, the place it was for, and the layer / chip heights in dp (ShellHost, from
   * ShellCallChromeGesture and the laid-out elements; each kept until a newer one is known). The chip's markup puts a
   * rebuilt chip there (ShellCallChromeRml); 0: not known yet.
   */
  CallChipLayout call_chip_layout;
  /** Top safe-area inset in dp (SDL and/or machine prefs); clears status bar / notch. */
  int safe_area_top_dp = 0;
  /** Bottom safe-area inset in dp (SDL and/or machine prefs). */
  int safe_area_bottom_dp = 0;

  /**
   * Phone AI dot (DESIGN-pp-ui §8.14; ShellAiDot). From profile prefs: Me → AI assistant → Show AI dot and the
   * assistant look (`a` one eye, `b` two eyes, `c` cool, `d` plain), its size (`s` 52dp, `m` 64dp, `l` 76dp; bound),
   * and where it rests (side, height in the band).
   */
  bool ai_dot_enabled = true;
  ui::String ai_dot_look = "b";
  ui::String ai_dot_size = "m";
  bool ai_dot_left = false;
  int ai_dot_y_permille = 600;
  /** Tucked half off its side by the user (flung or dragged into the edge); kept with the place. */
  bool ai_dot_tucked = false;
  /** Bound: the dot is on screen now; its action now (AiDotModel::IdleName: `sway`, `blink`, `look`, `talk`, `shy`,
   * `wave`, `listen`, `think`, `five`, `happy`, `private`, `sleep`, `poke`, `pop`, or empty); asleep (eyes shut, still) after
   * its sleep until the next touch. */
  bool ai_dot_shown = false;
  ui::String ai_dot_idle;
  bool ai_dot_asleep = false;

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
  /**
   * Full-bleed brand cover until interactive-ready (silent unlock / messaging).
   * Hidden while the PIN gate needs input. Armed from first paint until deferred
   * unlock settles; then follows unlock_in_progress only.
   */
  bool startup_cover_visible = true;
};

} // namespace pbr
