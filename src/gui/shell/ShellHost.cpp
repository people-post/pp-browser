#include <stdexcept>
#include <utility>
#include "gui/shell/ShellHost.h"

#include "lib/ui/DataEvent.h"
#include "foundation/runtime/FramePacing.h"

#include "common/Metrics.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/runtime/AppRuntime.h"
#include "foundation/platform/ui/DesktopWindowChrome.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/ScreenKeyboard.h"
#include "foundation/platform/ui/PlatformNavigation.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/RmlVariantHelpers.h"
#include "feature/conversations/MessagingShellPorts.h"
#include "gui/shell/DataModelHost.h"
#include "gui/CallActionsPorts.h"
#include "gui/FlowCoordinatorPorts.h"
#include "gui/PinGateActionPorts.h"
#include "gui/shell/RmlMount.h"
#include "gui/shell/ShellLayoutRml.h"
#include "gui/shell/ShellStatusbar.h"
#include "gui/shell/ShellCallChromeRml.h"
#include "gui/shell/ShellFeedback.h"
#include "lib/ui/ShellInterruption.h"
#include "lib/ui/ShellLayout.h"
#include "lib/ui/UiEditSession.h"
#include "lib/ui/ViewCatalog.h"

#include "foundation/platform/ui/RmlUi_Backend.h"

#include <ui/dom/Context.h>
#include <ui/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/base/Log.h>
#include <ui/base/SystemInterface.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#if UI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {

NavTab NavTabFromString(const ui::String& value) {
  if (value == "contacts") {
    return NavTab::Contacts;
  }
  if (value == "sessions") {
    return NavTab::Sessions;
  }
  if (value == "me") {
    return NavTab::Me;
  }
  return NavTab::Home;
}

/** Bottom inset at or above this is treated as IME (not home-indicator only). */
constexpr int kImeLatchMinDp = 120;
constexpr int64_t kSlowSyncLayoutMs = 30; // two 60 Hz frames
/** Fallback emoji panel height when the OSK has never been shown this session. */
constexpr int kDefaultEmojiKeyboardPanelDp = 280;

} // namespace

ShellHost::ShellHost() {
  redirectLogger("ShellHost");
}

void ShellHost::BindShellMessaging(MessagingShellPorts ports) {
  shell_messaging_ports_ = std::move(ports);
}

void ShellHost::BindPinGateActions(PinGateActionPorts ports) {
  pin_gate_actions_ = std::move(ports);
}

void ShellHost::BindFlowCoordinator(FlowCoordinatorPorts ports) {
  flow_coordinator_ = std::move(ports);
}

void ShellHost::BindCallActions(CallActionsPorts ports) {
  call_actions_ = std::move(ports);
}

bool ShellHost::RegisterWindowModel(ui::Context* context) {
  return DataModelHost::Instance().Register(context, "window", [this](ui::DataModelConstructor& ctor) {
    ShellHost& host = *this;
    if (auto toast_handle = ctor.RegisterStruct<ToastEntry>()) {
      toast_handle.RegisterMember("id", &ToastEntry::id);
      toast_handle.RegisterMember("message", &ToastEntry::message);
    }
    ctor.RegisterArray<std::vector<ToastEntry>>();

    ctor.Bind("layout_mode", &host.state_.layout_mode_str);
    ctor.Bind("nav_tab", &host.state_.nav_tab_str);
    ctor.Bind("compact_chat_open", &host.state_.compact_chat_open);
    ctor.Bind("account_sheet_open", &host.state_.account_sheet_open);
    ctor.Bind("auxiliary_open", &host.state_.auxiliary_open);
    ctor.Bind("sidebar_collapsed", &host.state_.sidebar_collapsed);
    ctor.Bind("auxiliary_available", &host.state_.auxiliary_available);
    ctor.Bind("transient_active", &host.state_.transient_active);
    ctor.Bind("banner_message", &host.state_.banner_message);
    ctor.Bind("toasts", &host.state_.toasts);
    ctor.Bind("dialog_active", &host.state_.dialog.active);
    ctor.Bind("dialog_title", &host.state_.dialog.title);
    ctor.Bind("dialog_message", &host.state_.dialog.message);
    ctor.Bind("dialog_show_cancel", &host.state_.dialog.show_cancel);
    ctor.Bind("dialog_show_checkbox", &host.state_.dialog.show_checkbox);
    ctor.Bind("dialog_checkbox_label", &host.state_.dialog.checkbox_label);
    ctor.Bind("dialog_checkbox_checked", &host.state_.dialog.checkbox_checked);
    ctor.Bind("dialog_show_prompt", &host.state_.dialog.show_prompt);
    ctor.Bind("dialog_prompt_value", &host.state_.dialog.prompt_value);
    ctor.Bind("pin_gate_active", &host.state_.pin_gate.active);
    ctor.Bind("pin_gate_identity_fork_mode", &host.state_.pin_gate.identity_fork_mode);
    ctor.Bind("pin_gate_restore_label", &host.state_.pin_gate.restore_label);
    ctor.Bind("pin_gate_chooser_mode", &host.state_.pin_gate.chooser_mode);
    ctor.Bind("pin_gate_create_mode", &host.state_.pin_gate.create_mode);
    ctor.Bind("pin_gate_link_paste_mode", &host.state_.pin_gate.link_paste_mode);
    ctor.Bind("pin_gate_restore_pin_mode", &host.state_.pin_gate.restore_pin_mode);
    ctor.Bind("pin_gate_unlock_mode", &host.state_.pin_gate.unlock_mode);
    ctor.Bind("pin_gate_forgot_open", &host.state_.pin_gate.forgot_open);
    ctor.Bind("pin_gate_title", &host.state_.pin_gate.title);
    ctor.Bind("pin_gate_message", &host.state_.pin_gate.message);
    ctor.Bind("pin_gate_error", &host.state_.pin_gate.error);
    ctor.Bind("pin_gate_pin", &host.state_.pin_gate.pin);
    ctor.Bind("pin_gate_pin_confirm", &host.state_.pin_gate.pin_confirm);
    ctor.Bind("pin_gate_link_payload", &host.state_.pin_gate.link_payload);
    ctor.Bind("call_ring_active", &host.state_.call_ring.active);
    ctor.Bind("call_ring_pulse", &host.state_.call_ring.pulse);
    ctor.Bind("call_ring_conflict", &host.state_.call_ring.conflict);
    ctor.Bind("call_ring_caller", &host.state_.call_ring.caller_label);
    ctor.Bind("call_ring_media", &host.state_.call_ring.media_label);
    ctor.Bind("call_ring_eyebrow", &host.state_.call_ring.eyebrow);
    ctor.Bind("call_ring_conflict_hint", &host.state_.call_ring.conflict_hint);
    ctor.Bind("call_ring_accept_label", &host.state_.call_ring.accept_label);
    ctor.Bind("call_ring_voice_answer_label", &host.state_.call_ring.voice_answer_label);
    ctor.Bind("call_ring_video_allowed", &host.state_.call_ring.video_allowed);
    ctor.Bind("call_ring_decline_label", &host.state_.call_ring.decline_label);
    ctor.Bind("call_ring_show_pricing", &host.state_.call_ring.show_pricing);
    ctor.Bind("call_ring_pricing_label", &host.state_.call_ring.pricing_label);
    ctor.Bind("call_ring_accept_charge_label", &host.state_.call_ring.accept_charge_label);
    ctor.Bind("call_ring_accept_charge_enabled", &host.state_.call_ring.accept_charge_enabled);
    ctor.Bind("call_ring_accept_charge_hint", &host.state_.call_ring.accept_charge_hint);
    ctor.Bind("call_ring_accept_short", &host.state_.call_ring.accept_short);
    ctor.Bind("call_ring_voice_short", &host.state_.call_ring.voice_short);
    ctor.Bind("call_in_progress_active", &host.state_.call_in_progress.active);
    ctor.Bind("call_in_progress_title", &host.state_.call_in_progress.title);
    ctor.Bind("call_in_progress_subtitle", &host.state_.call_in_progress.subtitle);
    ctor.Bind("call_in_progress_status_hint", &host.state_.call_in_progress.status_hint);
    ctor.Bind("call_in_progress_mic_level", &host.state_.call_in_progress.mic_level);
    ctor.Bind("call_in_progress_peer_level", &host.state_.call_in_progress.peer_level);
    ctor.Bind("call_in_progress_mic_hint", &host.state_.call_in_progress.mic_hint);
    ctor.Bind("call_in_progress_peer_hint", &host.state_.call_in_progress.peer_hint);
    ctor.Bind("call_in_progress_muted", &host.state_.call_in_progress.muted);
    ctor.Bind("call_in_progress_camera_on", &host.state_.call_in_progress.camera_on);
    ctor.Bind("call_in_progress_speaker_on", &host.state_.call_in_progress.speaker_on);
    ctor.Bind("call_in_progress_stage_visible", &host.state_.call_in_progress.stage_visible);
    ctor.Bind("call_in_progress_remote_video", &host.state_.call_in_progress.remote_video);
    ctor.Bind("call_in_progress_local_preview", &host.state_.call_in_progress.local_preview);
    ctor.Bind("call_in_progress_elapsed", &host.state_.call_in_progress.elapsed);
    ctor.Bind("call_in_progress_peer_label", &host.state_.call_in_progress.peer_label);
    ctor.Bind("call_in_progress_remote_placeholder", &host.state_.call_in_progress.remote_placeholder);
    ctor.Bind("call_in_progress_show_roster", &host.state_.call_in_progress.show_roster);
    ctor.Bind("call_in_progress_show_invite", &host.state_.call_in_progress.show_invite);
    ctor.Bind("call_in_progress_show_retry", &host.state_.call_in_progress.show_retry);
    ctor.Bind("call_in_progress_show_speaker", &host.state_.call_in_progress.show_speaker);
    ctor.Bind("call_in_progress_show_camera", &host.state_.call_in_progress.show_camera);
    ctor.Bind("call_in_progress_participant_count", &host.state_.call_in_progress.participant_count);
    ctor.Bind("call_in_progress_mode", &host.state_.call_in_progress.mode_str);
    ctor.Bind("call_in_progress_minimized_corner", &host.state_.call_in_progress.minimized_corner);
    ctor.Bind("call_in_progress_quality_bars", &host.state_.call_in_progress.quality_bars);
    ctor.Bind("call_in_progress_quality_ok", &host.state_.call_in_progress.quality_ok);
    ctor.Bind("call_in_progress_quality_warn", &host.state_.call_in_progress.quality_warn);
    ctor.Bind("call_in_progress_quality_error", &host.state_.call_in_progress.quality_error);
    ctor.Bind("call_in_progress_quality_label", &host.state_.call_in_progress.quality_label);
    ctor.Bind("call_in_progress_quality_hint", &host.state_.call_in_progress.quality_hint);
    ctor.Bind("call_in_progress_show_debug_subtitle", &host.state_.call_in_progress.show_debug_subtitle);
    ctor.Bind("call_in_progress_debug_subtitle", &host.state_.call_in_progress.debug_subtitle);
    if (auto roster_handle = ctor.RegisterStruct<CallRosterParticipantState>()) {
      roster_handle.RegisterMember("name", &CallRosterParticipantState::name);
      roster_handle.RegisterMember("stream_id", &CallRosterParticipantState::stream_id);
      roster_handle.RegisterMember("audio_muted", &CallRosterParticipantState::audio_muted);
      roster_handle.RegisterMember("video_enabled", &CallRosterParticipantState::video_enabled);
      roster_handle.RegisterMember("is_local", &CallRosterParticipantState::is_local);
      roster_handle.RegisterMember("has_remote_video", &CallRosterParticipantState::has_remote_video);
      roster_handle.RegisterMember("has_avatar", &CallRosterParticipantState::has_avatar);
      roster_handle.RegisterMember("avatar_src", &CallRosterParticipantState::avatar_src);
      roster_handle.RegisterMember("avatar_letter", &CallRosterParticipantState::avatar_letter);
      roster_handle.RegisterMember("avatar_tone", &CallRosterParticipantState::avatar_tone);
    }
    ctor.RegisterArray<std::vector<CallRosterParticipantState>>();
    ctor.Bind("call_in_progress_roster", &host.state_.call_in_progress.roster);
    ctor.Bind("activity_visible", &host.state_.activity_visible);
    statusbar_.Bind(ctor);
    ctor.Bind("statusbar_activity", &host.state_.statusbar_activity);
    ctor.Bind("titlebar_visible", &host.state_.titlebar_visible);
    ctor.Bind("titlebar_traffic_lights", &host.state_.titlebar_traffic_lights);
    ctor.Bind("window_maximized", &host.state_.window_maximized);
    ctor.Bind("fonts_ready", &host.state_.fonts_ready);
    ctor.Bind("unlock_in_progress", &host.state_.unlock_in_progress);
    ctor.Bind("startup_cover_visible", &host.state_.startup_cover_visible);

    if (auto badge_handle = ctor.RegisterStruct<NavBadgeState>()) {
      badge_handle.RegisterMember("sessions_unread", &NavBadgeState::sessions_unread);
      badge_handle.RegisterMember("contacts_unread", &NavBadgeState::contacts_unread);
      badge_handle.RegisterMember("me_attention", &NavBadgeState::me_attention);
      badge_handle.RegisterMember("sessions_unread_display", &NavBadgeState::sessions_unread_display);
      badge_handle.RegisterMember("contacts_unread_display", &NavBadgeState::contacts_unread_display);
    }
    ctor.Bind("nav_badges", &host.state_.nav_badges);
    // Top-level scalars: DirtyVariable("nav_badges") alone has not been reliable for data-if/data-rml
    // on nested struct members (Android needed remount/tab switches). Mirror the fields as scalars.
    ctor.Bind("nav_sessions_unread", &host.state_.nav_badges.sessions_unread);
    ctor.Bind("nav_contacts_unread", &host.state_.nav_badges.contacts_unread);
    ctor.Bind("nav_me_attention", &host.state_.nav_badges.me_attention);
    ctor.Bind("nav_sessions_unread_display", &host.state_.nav_badges.sessions_unread_display);
    ctor.Bind("nav_contacts_unread_display", &host.state_.nav_badges.contacts_unread_display);

    ctor.BindEventCallback("toggle_sidebar", DataEvent<>([this]() { ToggleSidebarCollapsed(); }));
    ctor.BindEventCallback("toggle_auxiliary", DataEvent<>([this]() { ToggleAuxiliary(); }));
    ctor.BindEventCallback("open_auxiliary", DataEvent<>([this]() { OpenAuxiliary(); }));
    ctor.BindEventCallback("select_nav_tab", DataEvent<std::string>([this](const std::string& tab) { OnNavTabClicked(tab); }));
    ctor.BindEventCallback("compact_chat_back", DataEvent<>([this]() { RequestDismiss(DismissStyle::Instant, DismissTarget::CompactChatOverlay); }));
    ctor.BindEventCallback("open_account_sheet", DataEvent<>([this]() { OpenAccountSheet(); }));
    // × always closes the sheet, even when a nested settings detail is open.
    ctor.BindEventCallback("close_account_sheet", DataEvent<>([this]() { CloseAccountSheet(); }));
    ctor.BindEventCallback("transient_back", DataEvent<>([this]() { RequestDismiss(DismissStyle::Instant, DismissTarget::Transient); }));
    ctor.BindEventCallback("close_layer", DataEvent<int>([this](int layer_id) { CloseLayer(layer_id); }));
    ctor.BindEventCallback("dismiss_banner", DataEvent<>([this]() { DismissBanner(); }));
    ctor.BindEventCallback("dialog_ok", DataEvent<>([this]() { DialogOk(); }));
    ctor.BindEventCallback("dialog_cancel", DataEvent<>([this]() { DialogCancel(); }));
    ctor.BindEventCallback("dialog_toggle_checkbox", DataEvent<>([this]() { DialogToggleCheckbox(); }));
    ctor.BindEventCallback("pin_gate_submit", DataEvent<>([this]() { PinGateSubmit(); }));
    ctor.BindEventCallback("pin_gate_cancel", DataEvent<>([this]() { PinGateCancel(); }));
    ctor.BindEventCallback("pin_gate_set_pin", DataEvent<>([this]() { PinGateSetPin(); }));
    ctor.BindEventCallback("pin_gate_use_default", DataEvent<>([this]() { PinGateUseDefault(); }));
    ctor.BindEventCallback("pin_gate_identity_new", DataEvent<>([this]() { PinGateIdentityNew(); }));
    ctor.BindEventCallback("pin_gate_identity_link", DataEvent<>([this]() { PinGateIdentityLink(); }));
    ctor.BindEventCallback("pin_gate_identity_restore", DataEvent<>([this]() { PinGateIdentityRestore(); }));
    ctor.BindEventCallback("pin_gate_forgot_toggle", DataEvent<>([this]() { PinGateForgotToggle(); }));
    ctor.BindEventCallback("call_accept", DataEvent<>([this]() { CallAccept(); }));
    ctor.BindEventCallback("call_accept_voice", DataEvent<>([this]() { CallAcceptVoice(); }));
    ctor.BindEventCallback("call_accept_charge", DataEvent<>([this]() { CallAcceptCharge(); }));
    ctor.BindEventCallback("call_decline", DataEvent<>([this]() { CallDecline(); }));
    ctor.BindEventCallback("call_leave", DataEvent<>([this]() { CallLeave(); }));
    ctor.BindEventCallback("call_retry", DataEvent<>([this]() { CallRetry(); }));
    ctor.BindEventCallback("call_mute", DataEvent<>([this]() { CallMute(); }));
    ctor.BindEventCallback("call_camera", DataEvent<>([this]() { CallCamera(); }));
    ctor.BindEventCallback("call_speaker", DataEvent<>([this]() { CallSpeaker(); }));
    ctor.BindEventCallback("call_invite", DataEvent<>([this]() { CallInvite(); }));
    ctor.BindEventCallback("call_minimize", DataEvent<>([this]() { CallMinimize(); }));
    ctor.BindEventCallback("call_expand", DataEvent<>([this]() { CallExpand(); }));
    ctor.BindEventCallback("call_immersive", DataEvent<>([this]() { CallImmersive(); }));
    ctor.BindEventCallback("call_restore", DataEvent<>([this]() { CallRestore(); }));
    ctor.BindEventCallback("call_details", DataEvent<>([this]() { CallDetails(); }));
    ctor.BindEventCallback("titlebar_minimize", DataEvent<>([this]() { TitlebarMinimize(); }));
    ctor.BindEventCallback("titlebar_toggle_maximize", DataEvent<>([this]() { TitlebarToggleMaximize(); }));
    ctor.BindEventCallback("titlebar_close", DataEvent<>([this]() { TitlebarClose(); }));
  });
}

void ShellHost::Initialize(ui::Context* context) {
  context_ = context;
  state_ = {};
  state_.layout_mode = LayoutMode::Expanded;
  state_.titlebar_visible = DesktopWindowChrome::Enabled();
  state_.titlebar_traffic_lights = DesktopWindowChrome::ControlsLeading();
  state_.window_maximized = DesktopWindowChrome::IsMaximized();
  ShellLayout::SyncLayoutModeString(state_);
  ShellLayout::SyncNavTabString(state_);
  statusbar_.RefreshVisibility();
  last_synced_mode_ = LayoutMode::Expanded;
  next_pane_id_ = 1;
  next_overlay_id_ = 1;
  elapsed_ms_ = 0.f;
  pending_dismiss_.reset();
  local_back_stack_.clear();
  DetachDismissGestures();
  saved_focus_id_.clear();
  sync_pending_ = false;
  restore_focus_after_sync_ = false;
  PlatformNavigation::SetDismissHandler([this] { return HandleDismiss(); });
  if (state_.titlebar_traffic_lights) {
    // Leading traffic lights: ~8+12×3+8×2+8 ≈ 68dp (native-like spacing).
    config_.titlebar_controls_width_dp = 68.f;
  }
  if (state_.titlebar_visible) {
    DesktopWindowChrome::Install();
    DesktopWindowChrome::SetLayout(config_.titlebar_height_dp, config_.titlebar_controls_width_dp,
                                   5.f, state_.titlebar_traffic_lights);
  }
}

ui::Element* ShellHost::ShellRoot() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return nullptr;
  }
  return context_->GetDocument(0)->GetElementById("shell-root");
}

int ShellHost::AllocatePaneId() {
  return next_pane_id_++;
}

int ShellHost::AllocateOverlayId() {
  return next_overlay_id_++;
}

const PaneState* ShellHost::FindPane(const std::string& key) const {
  for (const PaneState& pane : state_.panes) {
    if (pane.spec.key == key) {
      return &pane;
    }
  }
  return nullptr;
}

void ShellHost::RegisterPane(const PaneSpec& spec) {
  if (FindPane(spec.key)) {
    return;
  }
  PaneState pane;
  pane.spec = spec;
  pane.id = AllocatePaneId();
  state_.panes.push_back(std::move(pane));
}

void ShellHost::SetAuxiliaryAvailable(bool available) {
  const bool was_available = state_.auxiliary_available;
  state_.auxiliary_available = available;
  if (!available) {
    state_.auxiliary_open = false;
  }
  DirtyNavChrome();
  // Compact: callers open the sheet via OpenAuxiliary() when appropriate. Do not toast
  // "tap Preview" here — that message stuck on mobile when the fake frame clock stalled,
  // and it conflicts with auto-open. Expanded still opens the side pane immediately.
  if (available && !was_available && state_.layout_mode == LayoutMode::Expanded) {
    state_.auxiliary_open = true;
    RequestSyncLayout();
  }
}

void ShellHost::OpenAuxiliary() {
  if (!state_.auxiliary_available) {
    return;
  }
  state_.auxiliary_open = true;
  RequestSyncLayout();
}

void ShellHost::CloseAuxiliary() {
  state_.auxiliary_open = false;
  RequestSyncLayout();
}

void ShellHost::ClearTabContext() {
  state_.primary_pane_key.clear();
  state_.auxiliary_open = false;
  state_.transient_stack.clear();
  state_.transient_active = false;
  state_.compact_chat_open = false;
  pending_dismiss_.reset();
  local_back_stack_.clear();
  DetachDismissGestures();
  if (state_.account_sheet_open) {
    state_.account_sheet_open = false;
    if (on_account_sheet_closed_) {
      on_account_sheet_closed_();
    }
  }
}

void ShellHost::SetPrimaryPane(const std::string& key) {
  state_.primary_pane_key = key.c_str();
  RequestSyncLayout();
}

void ShellHost::ClearPrimaryPane() {
  SetPrimaryPane("");
}

void ShellHost::SelectNavTab(NavTab tab) {
  if (state_.unlock_in_progress && tab != NavTab::Home) {
    ShellFeedback::ShowToast(state_, Tr("startup.still_preparing"));
    DirtyFeedback();
    return;
  }
  // Compact Me stays an account bottom sheet (same as before), not a list/detail tab.
  if (tab == NavTab::Me && state_.layout_mode == LayoutMode::Compact) {
    OpenAccountSheet();
    return;
  }
  if (tab == NavTab::Me && state_.account_sheet_open) {
    CloseAccountSheet();
  }
  const bool tab_changed = state_.nav_tab != tab;
  if (tab_changed) {
    ClearTabContext();
  }
  state_.nav_tab = tab;
  ShellLayout::SyncNavTabString(state_);
  if (tab_changed && on_nav_tab_changed_) {
    on_nav_tab_changed_(tab);
  }
  RequestSyncLayout();
}

void ShellHost::OpenCompactChat() {
  if (state_.layout_mode != LayoutMode::Compact) {
    return;
  }
  state_.compact_chat_open = true;
  if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::CompactChatOverlay) {
    pending_dismiss_.reset();
  }
  RequestSyncLayout();
}

void ShellHost::CloseCompactChat() {
  if (!state_.compact_chat_open) {
    return;
  }
  state_.compact_chat_open = false;
  if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::CompactChatOverlay) {
    pending_dismiss_.reset();
  }
  DetachDismissGestures();
  RequestSyncLayout();
}

void ShellHost::OpenAccountSheet() {
  if (state_.unlock_in_progress) {
    ShellFeedback::ShowToast(state_, Tr("startup.still_preparing"));
    DirtyFeedback();
    return;
  }
  if (state_.account_sheet_open) {
    return;
  }
  state_.account_sheet_open = true;
  if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::AccountSheet) {
    pending_dismiss_.reset();
  }
  if (on_account_sheet_opened_) {
    on_account_sheet_opened_();
  }
  RequestSyncLayout();
}

void ShellHost::CloseAccountSheet() {
  if (!state_.account_sheet_open) {
    return;
  }
  state_.account_sheet_open = false;
  if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::AccountSheet) {
    pending_dismiss_.reset();
  }
  local_back_stack_.clear();
  DetachDismissGestures();
  if (on_account_sheet_closed_) {
    on_account_sheet_closed_();
  }
  RequestSyncLayout();
}

void ShellHost::SetSidebarPrefs(int width_dp, bool collapsed) {
  const int clamped = ShellLayout::ClampSidebarWidthDp(width_dp);
  if (state_.sidebar_width_dp == clamped && state_.sidebar_collapsed == collapsed) {
    return;
  }
  state_.sidebar_width_dp = clamped;
  state_.sidebar_collapsed = collapsed;
  // The shell may already be mounted with the defaults; remount so the pane and the splitter's
  // starting width use the stored values.
  DirtyNavChrome();
  RequestSyncLayout();
}

void ShellHost::SetOnSidebarChanged(std::function<void(int width_dp, bool collapsed)> callback) {
  on_sidebar_changed_ = std::move(callback);
}

void ShellHost::SetAuxiliaryWidthPref(int width_dp) {
  const int clamped = ShellLayout::ClampAuxiliaryWidthDp(width_dp);
  if (state_.auxiliary_width_dp == clamped) {
    return;
  }
  state_.auxiliary_width_dp = clamped;
  // Remount so an already-open auxiliary pane and the splitter's starting width use the stored value.
  RequestSyncLayout();
}

void ShellHost::SetOnAuxiliaryWidthChanged(std::function<void(int width_dp)> callback) {
  on_auxiliary_width_changed_ = std::move(callback);
}

void ShellHost::NotifySidebarChanged() {
  if (on_sidebar_changed_) {
    on_sidebar_changed_(state_.sidebar_width_dp, state_.sidebar_collapsed);
  }
}

void ShellHost::ToggleSidebarCollapsed() {
  if (state_.layout_mode != LayoutMode::Expanded || !ShellLayout::TabHasSecondary(state_.nav_tab)) {
    return;
  }
  state_.sidebar_collapsed = !state_.sidebar_collapsed;
  NotifySidebarChanged();
  DirtyNavChrome();
  RequestSyncLayout();
}

void ShellHost::ToggleAuxiliary() {
  if (!state_.auxiliary_available) {
    return;
  }
  state_.auxiliary_open = !state_.auxiliary_open;
  RequestSyncLayout();
}

void ShellHost::PushTransient(const PaneSpec& spec) {
  PaneState pane;
  pane.spec = spec;
  pane.spec.role = PaneRole::Transient;
  pane.id = AllocatePaneId();
  state_.transient_stack.push_back(std::move(pane));
  state_.transient_active = true;
  RequestSyncLayout();
}

void ShellHost::PopTransient() {
  if (state_.transient_stack.empty()) {
    return;
  }
  const std::string popped_key = state_.transient_stack.back().spec.key;
  state_.transient_stack.pop_back();
  state_.transient_active = !state_.transient_stack.empty();
  if (on_transient_popped_) {
    on_transient_popped_(popped_key);
  }
  RequestSyncLayout();
}

int ShellHost::PushLayer(const PaneSpec& spec) {
  OverlayEntry entry;
  entry.id = AllocateOverlayId();
  entry.kind = OverlayKind::Generic;
  entry.key = spec.key;
  entry.rml_path = spec.rml_path.empty() ? ViewCatalog::ResolvePath(spec.key) : spec.rml_path;
  state_.overlay_stack.push_back(std::move(entry));
  if (!spec.return_focus_id.empty()) {
    saved_focus_id_ = spec.return_focus_id.c_str();
  } else {
    SaveFocus();
  }
  RequestSyncLayout();
  return state_.overlay_stack.back().id;
}

void ShellHost::CloseLayer(int layer_id) {
  if (state_.overlay_stack.empty()) {
    return;
  }
  const int closing_id = layer_id < 0 ? state_.overlay_stack.back().id : layer_id;
  if (flow_coordinator_.notify_layer_closing) {
    flow_coordinator_.notify_layer_closing(closing_id);
  }
  if (layer_id < 0) {
    state_.overlay_stack.pop_back();
  } else {
    state_.overlay_stack.erase(
        std::remove_if(state_.overlay_stack.begin(), state_.overlay_stack.end(),
                       [layer_id](const OverlayEntry& entry) { return entry.id == layer_id; }),
        state_.overlay_stack.end());
  }
  RequestSyncLayout(true);
}

DismissTarget ShellHost::ResolveDismissTarget() const {
  if (!local_back_stack_.empty()) {
    return DismissTarget::LocalBack;
  }
  switch (ShellInterruption::Top(state_)) {
  case InterruptionKind::OverlayLayer:
    return DismissTarget::OverlayLayer;
  case InterruptionKind::Transient:
    return DismissTarget::Transient;
  case InterruptionKind::AccountSheet:
    return DismissTarget::AccountSheet;
  case InterruptionKind::AuxiliarySheet:
    return DismissTarget::AuxiliarySheet;
  case InterruptionKind::CompactChatOverlay:
    return DismissTarget::CompactChatOverlay;
  case InterruptionKind::Dialog:
  case InterruptionKind::PinGate:
  case InterruptionKind::CallRing:
  case InterruptionKind::CallInProgress:
  case InterruptionKind::None:
    return DismissTarget::None;
  }
  return DismissTarget::None;
}

void ShellHost::BeginAnimatedDismiss(DismissTarget target) {
  pending_dismiss_ = PendingDismiss{
      .target = target,
      .at = std::chrono::steady_clock::now() + std::chrono::milliseconds(220),
  };
}

void ShellHost::CommitDismiss(DismissTarget target) {
  pending_dismiss_.reset();
  switch (target) {
  case DismissTarget::None:
    return;
  case DismissTarget::LocalBack: {
    if (local_back_stack_.empty()) {
      return;
    }
    LocalBackEntry entry = std::move(local_back_stack_.back());
    local_back_stack_.pop_back();
    if (entry.commit) {
      entry.commit();
    }
    // commit schedules RefreshDismissGestures after the detail DOM updates.
    DirtyNavChrome();
    return;
  }
  case DismissTarget::CompactChatOverlay:
    CloseCompactChat();
    return;
  case DismissTarget::AuxiliarySheet:
    CloseAuxiliary();
    return;
  case DismissTarget::AccountSheet:
    CloseAccountSheet();
    return;
  case DismissTarget::Transient:
    PopTransient();
    return;
  case DismissTarget::OverlayLayer:
    CloseLayer();
    return;
  }
}

bool ShellHost::RequestDismiss(DismissStyle style, DismissTarget force) {
  if (pending_dismiss_) {
    return true;
  }
  const DismissTarget target = (force != DismissTarget::None) ? force : ResolveDismissTarget();
  if (target == DismissTarget::None) {
    return false;
  }
  if (style == DismissStyle::Animated) {
    BeginAnimatedDismiss(target);
    return true;
  }
  CommitDismiss(target);
  return true;
}

bool ShellHost::HandleDismiss() {
  if (ContextMenuHost::Instance().HandleDismiss()) {
    return true;
  }
  if (state_.statusbar_popover_open) {
    statusbar_.ClosePopover();
    return true;
  }
  if (state_.dialog.active) {
    if (state_.dialog.show_cancel) {
      ShellFeedback::DialogCancel(state_);
    } else {
      ShellFeedback::DialogOk(state_);
    }
    return true;
  }
  if (state_.pin_gate.active) {
    if (state_.pin_gate.create_mode || state_.pin_gate.chooser_mode || state_.pin_gate.identity_fork_mode ||
        state_.pin_gate.link_paste_mode || state_.pin_gate.restore_pin_mode) {
      if (pin_gate_actions_.on_cancel) {
        pin_gate_actions_.on_cancel();
      }
    }
    // Unlock: consume Escape without dismissing or quitting.
    return true;
  }
  if (bottom_chrome_open_) {
    if (on_bottom_chrome_dismissed_) {
      on_bottom_chrome_dismissed_();
    } else {
      ClearBottomChrome();
    }
    return true;
  }
  if (flow_coordinator_.handle_dismiss && flow_coordinator_.handle_dismiss()) {
    RequestSyncLayout();
    return true;
  }
  return RequestDismiss(DismissStyle::Instant);
}

void ShellHost::PushLocalBack(const std::string& id, std::function<void()> commit) {
  if (id.empty()) {
    return;
  }
  ClearLocalBack(id);
  local_back_stack_.push_back(LocalBackEntry{id, std::move(commit)});
  RefreshDismissGestures();
}

bool ShellHost::ClearLocalBack(const std::string& id) {
  const auto before = local_back_stack_.size();
  local_back_stack_.erase(
      std::remove_if(local_back_stack_.begin(), local_back_stack_.end(),
                     [&id](const LocalBackEntry& entry) { return entry.id == id; }),
      local_back_stack_.end());
  if (local_back_stack_.size() != before) {
    if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::LocalBack) {
      pending_dismiss_.reset();
    }
    RefreshDismissGestures();
    return true;
  }
  return false;
}

bool ShellHost::HasLocalBack(const std::string& id) const {
  return std::any_of(local_back_stack_.begin(), local_back_stack_.end(),
                     [&id](const LocalBackEntry& entry) { return entry.id == id; });
}

void ShellHost::RefreshDismissGestures() {
  DetachDismissGestures();
  if (state_.account_sheet_open) {
    AttachAccountSheetGesture();
  }
  AttachSwipeBackGesture();
}

void ShellHost::DirtyNavChrome() {
  DataModelHost::Instance().Dirty("window", "layout_mode");
  DataModelHost::Instance().Dirty("window", "nav_tab");
  DataModelHost::Instance().Dirty("window", "nav_badges");
  DataModelHost::Instance().Dirty("window", "nav_sessions_unread");
  DataModelHost::Instance().Dirty("window", "nav_contacts_unread");
  DataModelHost::Instance().Dirty("window", "nav_me_attention");
  DataModelHost::Instance().Dirty("window", "nav_sessions_unread_display");
  DataModelHost::Instance().Dirty("window", "nav_contacts_unread_display");
  DataModelHost::Instance().Dirty("window", "compact_chat_open");
  DataModelHost::Instance().Dirty("window", "account_sheet_open");
  DataModelHost::Instance().Dirty("window", "auxiliary_open");
  DataModelHost::Instance().Dirty("window", "sidebar_collapsed");
  DataModelHost::Instance().Dirty("window", "auxiliary_available");
  DataModelHost::Instance().Dirty("window", "transient_active");
}

void ShellHost::DirtyFeedback() {
  DataModelHost::Instance().Dirty("window", "banner_message");
  DataModelHost::Instance().Dirty("window", "toasts");
  DataModelHost::Instance().Dirty("window", "dialog_active");
  DataModelHost::Instance().Dirty("window", "dialog_title");
  DataModelHost::Instance().Dirty("window", "dialog_message");
  DataModelHost::Instance().Dirty("window", "dialog_show_cancel");
  DataModelHost::Instance().Dirty("window", "dialog_show_checkbox");
  DataModelHost::Instance().Dirty("window", "dialog_checkbox_label");
  DataModelHost::Instance().Dirty("window", "dialog_checkbox_checked");
  DataModelHost::Instance().Dirty("window", "dialog_show_prompt");
  DataModelHost::Instance().Dirty("window", "dialog_prompt_value");
}

void ShellHost::DirtyPinGate() {
  DataModelHost::Instance().Dirty("window", "pin_gate_active");
  DataModelHost::Instance().Dirty("window", "pin_gate_identity_fork_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_restore_label");
  DataModelHost::Instance().Dirty("window", "pin_gate_chooser_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_create_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_link_paste_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_restore_pin_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_unlock_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_forgot_open");
  DataModelHost::Instance().Dirty("window", "pin_gate_title");
  DataModelHost::Instance().Dirty("window", "pin_gate_message");
  DataModelHost::Instance().Dirty("window", "pin_gate_error");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin_confirm");
  DataModelHost::Instance().Dirty("window", "pin_gate_link_payload");
  DataModelHost::Instance().Dirty("window", "unlock_in_progress");
  DataModelHost::Instance().Dirty("window", "startup_cover_visible");
}

void ShellHost::DirtyWindow() {
  DirtyNavChrome();
  DirtyFeedback();
  DirtyPinGate();
  DirtyCallChrome();
  statusbar_.Dirty();
}

void ShellHost::DirtyCallChrome() {
  DataModelHost::Instance().Dirty("window", "call_ring_active");
  DataModelHost::Instance().Dirty("window", "call_ring_pulse");
  DataModelHost::Instance().Dirty("window", "call_ring_conflict");
  DataModelHost::Instance().Dirty("window", "call_ring_caller");
  DataModelHost::Instance().Dirty("window", "call_ring_media");
  DataModelHost::Instance().Dirty("window", "call_ring_eyebrow");
  DataModelHost::Instance().Dirty("window", "call_ring_conflict_hint");
  DataModelHost::Instance().Dirty("window", "call_ring_accept_label");
  DataModelHost::Instance().Dirty("window", "call_ring_voice_answer_label");
  DataModelHost::Instance().Dirty("window", "call_ring_video_allowed");
  DataModelHost::Instance().Dirty("window", "call_ring_decline_label");
  DataModelHost::Instance().Dirty("window", "call_ring_show_pricing");
  DataModelHost::Instance().Dirty("window", "call_ring_pricing_label");
  DataModelHost::Instance().Dirty("window", "call_ring_accept_charge_label");
  DataModelHost::Instance().Dirty("window", "call_ring_accept_charge_enabled");
  DataModelHost::Instance().Dirty("window", "call_ring_accept_charge_hint");
  DataModelHost::Instance().Dirty("window", "call_ring_accept_short");
  DataModelHost::Instance().Dirty("window", "call_ring_voice_short");
  DataModelHost::Instance().Dirty("window", "call_in_progress_active");
  DataModelHost::Instance().Dirty("window", "call_in_progress_title");
  DataModelHost::Instance().Dirty("window", "call_in_progress_subtitle");
  DataModelHost::Instance().Dirty("window", "call_in_progress_status_hint");
  DataModelHost::Instance().Dirty("window", "call_in_progress_mic_level");
  DataModelHost::Instance().Dirty("window", "call_in_progress_peer_level");
  DataModelHost::Instance().Dirty("window", "call_in_progress_mic_hint");
  DataModelHost::Instance().Dirty("window", "call_in_progress_peer_hint");
  DataModelHost::Instance().Dirty("window", "call_in_progress_muted");
  DataModelHost::Instance().Dirty("window", "call_in_progress_camera_on");
  DataModelHost::Instance().Dirty("window", "call_in_progress_speaker_on");
  DataModelHost::Instance().Dirty("window", "call_in_progress_stage_visible");
  DataModelHost::Instance().Dirty("window", "call_in_progress_remote_video");
  DataModelHost::Instance().Dirty("window", "call_in_progress_local_preview");
  DataModelHost::Instance().Dirty("window", "call_in_progress_elapsed");
  DataModelHost::Instance().Dirty("window", "call_in_progress_peer_label");
  DataModelHost::Instance().Dirty("window", "call_in_progress_remote_placeholder");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_roster");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_invite");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_retry");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_speaker");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_camera");
  DataModelHost::Instance().Dirty("window", "call_in_progress_participant_count");
  DataModelHost::Instance().Dirty("window", "call_in_progress_roster");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_bars");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_ok");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_warn");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_error");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_label");
  DataModelHost::Instance().Dirty("window", "call_in_progress_quality_hint");
  DataModelHost::Instance().Dirty("window", "call_in_progress_show_debug_subtitle");
  DataModelHost::Instance().Dirty("window", "call_in_progress_debug_subtitle");
}

void ShellHost::ApplyCallChromeSnapshot(const CallChromeSnapshot& snapshot, CallChromeUpdate update) {
  state_.call_ring = snapshot.ring;
  state_.call_in_progress = snapshot.in_progress;
  // Layer appear/disappear: mount into #shell-call-*-mount only (never full SyncLayout —
  // that remounts chat panes and broke Samsung Accept hit-testing).
  if (update == CallChromeUpdate::Remount) {
    RemountCallChrome();
  } else if (update == CallChromeUpdate::DirtyOnly) {
    DirtyCallChrome();
  }
  // Force Present so ring/accept chrome is not held behind idle wait (THREADING.md UI delivery).
  Backend::RequestForceFrame();
}

void ShellHost::ApplyPinGateState(const PinGateState& state) {
  state_.pin_gate = state;
  ReconcileStartupCover();
}

void ShellHost::ReconcileStartupCover() {
  // Cold-start only: once arm is settled and unlock is idle, never re-show (mid-session
  // EnsureUnlocked / messaging prepare must not flash the logo).
  if (!startup_cover_armed_ && !state_.unlock_in_progress) {
    startup_cover_enabled_ = false;
  }
  // PIN / identity chooser must be reachable; logo cover only masks silent prepare.
  const bool show = startup_cover_enabled_ && !state_.pin_gate.active &&
                    (startup_cover_armed_ || state_.unlock_in_progress);
  if (state_.startup_cover_visible == show) {
    return;
  }
  state_.startup_cover_visible = show;
  DataModelHost::Instance().Dirty("window", "startup_cover_visible");
  Backend::RequestForceFrame();
}

void ShellHost::SettleStartupCoverArm() {
  startup_cover_armed_ = false;
  ReconcileStartupCover();
}

void ShellHost::RequestRemountNavRail() {
  AppRuntime::PostUI([this]() {
    ShellHost& host = *this;
    // Compact chat overlay omits the rail from the DOM.
    if (host.state_.layout_mode == LayoutMode::Compact && host.state_.compact_chat_open) {
      return;
    }
    host.MountNavRail();
    host.DirtyNavChrome();
  });
}

void ShellHost::SetActivityVisible(bool visible) {
  SetActivity(visible);
}

void ShellHost::SetActivity(bool visible, const ui::String& message) {
  state_.activity_visible = visible;
  if (!visible) {
    state_.statusbar_activity.clear();
  } else if (!message.empty()) {
    state_.statusbar_activity = message;
  } else {
    state_.statusbar_activity = Tr("chat.thinking").c_str();
  }
  statusbar_.Dirty();
}

void ShellHost::SetOnBeforeTransientMount(std::function<void(const std::string& key)> callback) {
  on_before_transient_mount_ = std::move(callback);
}

void ShellHost::SetOnTransientMounted(std::function<void(const std::string& key)> callback) {
  on_transient_mounted_ = std::move(callback);
}

void ShellHost::SetOnTransientPopped(std::function<void(const std::string& key)> callback) {
  on_transient_popped_ = std::move(callback);
}

void ShellHost::SetOnNavTabChanged(std::function<void(NavTab tab)> callback) {
  on_nav_tab_changed_ = std::move(callback);
}

void ShellHost::SaveFocus() {
  saved_focus_id_.clear();
  if (!context_) {
    return;
  }
  if (ui::Element* focus = context_->GetFocusElement()) {
    saved_focus_id_ = focus->GetId();
  }
}

void ShellHost::RestoreFocus() {
  if (!context_ || saved_focus_id_.empty() || context_->GetNumDocuments() == 0) {
    saved_focus_id_.clear();
    return;
  }
  if (ui::Element* element = context_->GetDocument(0)->GetElementById(saved_focus_id_)) {
    element->Focus();
  }
  saved_focus_id_.clear();
}

void ShellHost::RequestSyncLayout(bool restore_focus_after, const char* reason, const std::source_location where) {
  std::string who = reason && reason[0] ? std::string(reason) : std::string();
  if (who.empty()) {
    // The call site, file name only (the full path is the build machine's).
    const std::string_view file = where.file_name();
    const size_t slash = file.find_last_of("/\\");
    who = std::string(slash == std::string_view::npos ? file : file.substr(slash + 1)) + ":" + std::to_string(where.line());
  }
  log().debug << "RequestSyncLayout reason=" << who << " pending=" << (sync_pending_ ? 1 : 0)
              << " restore_focus=" << (restore_focus_after ? 1 : 0);
  if (pending_sync_requests_++ == 0) {
    pending_sync_reason_ = who;
  }
  if (restore_focus_after) {
    restore_focus_after_sync_ = true;
  }
  if (sync_pending_) {
    // Flush already queued — still poke the event loop (X11 WaitEventTimeout could ignore
    // timeout; Sessions-tab click was previously the only unblock).
    Backend::RequestForceFrame();
    return;
  }
  sync_pending_ = true;
  // Always defer: SyncLayout remounts the shell DOM. Flushing synchronously from a click
  // handler (e.g. compact_chat_back) destroys the target element mid-dispatch and crashes.
  AppRuntime::PostUI([this]() { FlushPendingSyncLayout(); });
  Backend::RequestForceFrame();
}

void ShellHost::FlushPendingSyncLayout() {
  if (!sync_pending_) {
    return;
  }
  sync_pending_ = false;
  running_sync_reason_ = std::exchange(pending_sync_reason_, std::string());
  running_sync_requests_ = std::exchange(pending_sync_requests_, 0);
  try {
    SyncLayout();
  } catch (const std::exception& e) {
    log().error << "SyncLayout failed: " << e.what();
  } catch (...) {
    log().error << "SyncLayout failed with unknown exception";
  }
  if (restore_focus_after_sync_) {
    restore_focus_after_sync_ = false;
    RestoreFocus();
  }
}

void ShellHost::ApplyLayoutModeFromContext(ui::Context* context) {
  if (!context) {
    return;
  }
  const float dp_ratio = context->GetDensityIndependentPixelRatio();
  const float width_dp = static_cast<float>(context->GetDimensions().x) / dp_ratio;
  state_.shell_width_dp = width_dp;
  state_.layout_mode = ShellLayout::FromWidth(width_dp, config_.compact_breakpoint_dp);
  ShellLayout::SyncLayoutModeString(state_);
  ContextMenuHost::Instance().SetCompactLayout(state_.layout_mode == LayoutMode::Compact);
}

void ShellHost::SetOnLayoutModeChanged(std::function<void(LayoutMode mode)> callback) {
  on_layout_mode_changed_ = std::move(callback);
}

void ShellHost::SetOnLayoutSynced(std::function<void()> callback) {
  on_layout_synced_ = std::move(callback);
}

void ShellHost::SetOnAccountSheetOpened(std::function<void()> callback) {
  on_account_sheet_opened_ = std::move(callback);
}

void ShellHost::SetOnAccountSheetClosed(std::function<void()> callback) {
  on_account_sheet_closed_ = std::move(callback);
}

void ShellHost::SetSafeAreaInsetsFromPrefs(int top_dp, int bottom_dp) {
  safe_area_top_from_prefs_dp_ = std::max(0, top_dp);
  safe_area_bottom_from_prefs_dp_ = std::max(0, bottom_dp);
  state_.safe_area_top_dp = std::max(state_.safe_area_top_dp, safe_area_top_from_prefs_dp_);
  state_.safe_area_bottom_dp = std::max(state_.safe_area_bottom_dp, safe_area_bottom_from_prefs_dp_);
  ApplySafeAreaLayout();
}

ShellHost::SafeAreaFromSdl ShellHost::ReadSafeAreaFromSdl() const {
  SafeAreaFromSdl insets{};
#if UI_SDL_VERSION_MAJOR >= 3
  // SDL_GetWindowSafeArea is in window coordinates (points on iOS, pixels on Android).
  // RmlUi "dp" values are multiplied by display scale into framebuffer pixels — convert
  // so both platforms clear the same physical inset.
  SDL_Window* window = Backend::GetWindow();
  if (!window) {
    return insets;
  }
  SDL_Rect safe{};
  if (!SDL_GetWindowSafeArea(window, &safe)) {
    return insets;
  }
  int win_w = 0;
  int win_h = 0;
  if (!SDL_GetWindowSize(window, &win_w, &win_h) || win_h <= 0) {
    return insets;
  }
  const float density = SDL_GetWindowPixelDensity(window);
  const float display_scale = SDL_GetWindowDisplayScale(window);
  // context_px = window_units * density; dp = context_px / display_scale
  const float window_to_dp =
      (display_scale > 0.f) ? ((density > 0.f ? density : 1.f) / display_scale) : 1.f;
  const int top_win = std::max(0, safe.y);
  // iOS leaves the on-screen keyboard out of the safe area; it is reported separately (0 elsewhere).
  const int bottom_win = std::max({0, win_h - (safe.y + safe.h), ScreenKeyboardHeight()});
  insets.top_dp = static_cast<int>(static_cast<float>(top_win) * window_to_dp + 0.5f);
  insets.bottom_dp = static_cast<int>(static_cast<float>(bottom_win) * window_to_dp + 0.5f);
#endif
  return insets;
}

void ShellHost::RefreshSafeAreaInsets(ui::Context* context) {
  (void)context;
  const SafeAreaFromSdl sdl = ReadSafeAreaFromSdl();
  if (sdl.bottom_dp >= kImeLatchMinDp) {
    last_ime_bottom_dp_ = sdl.bottom_dp;
    if (bottom_chrome_open_) {
      bottom_chrome_height_dp_ = std::max(bottom_chrome_height_dp_, last_ime_bottom_dp_);
    }
  }

  const int effective_top = std::max(sdl.top_dp, safe_area_top_from_prefs_dp_);
  int effective_bottom = std::max(sdl.bottom_dp, safe_area_bottom_from_prefs_dp_);
  if (bottom_chrome_open_) {
    // Keep the shell lifted at the bottom-chrome height while the OSK is dismissed.
    effective_bottom = std::max(effective_bottom, bottom_chrome_height_dp_);
  }
  if (effective_top == state_.safe_area_top_dp && effective_bottom == state_.safe_area_bottom_dp) {
    if (bottom_chrome_open_) {
      ApplySafeAreaLayout();
    }
    return;
  }
  state_.safe_area_top_dp = effective_top;
  state_.safe_area_bottom_dp = effective_bottom;
  ApplySafeAreaLayout();
}

bool ShellHost::UsesBottomChromePresentation() const {
  return Platform::IsMobile() || state_.layout_mode == LayoutMode::Compact;
}

void ShellHost::SetOnBottomChromeDismissed(std::function<void()> callback) {
  on_bottom_chrome_dismissed_ = std::move(callback);
}

bool ShellHost::SetBottomChrome(const BottomChromeSpec& spec) {
  if (!UsesBottomChromePresentation() || spec.key.empty()) {
    return false;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->DeactivateKeyboard();
  }
  bottom_chrome_open_ = true;
  bottom_chrome_key_ = spec.key;
  bottom_chrome_rml_path_ = spec.rml_path.empty() ? ViewCatalog::ResolvePath(spec.key) : spec.rml_path;
  bottom_chrome_height_dp_ =
      std::max(last_ime_bottom_dp_ > 0 ? last_ime_bottom_dp_ : 0, kDefaultEmojiKeyboardPanelDp);
  state_.safe_area_bottom_dp = std::max(state_.safe_area_bottom_dp, bottom_chrome_height_dp_);
  RemountBottomChrome();
  ApplySafeAreaLayout();
  Backend::RequestForceFrame();
  return true;
}

void ShellHost::ClearBottomChrome() {
  if (!bottom_chrome_open_) {
    return;
  }
  bottom_chrome_open_ = false;
  bottom_chrome_height_dp_ = 0;
  bottom_chrome_key_.clear();
  bottom_chrome_rml_path_.clear();
  RemountBottomChrome();
  // Re-read SDL insets now that the synthetic panel is gone.
  RefreshSafeAreaInsets(context_);
  Backend::RequestForceFrame();
}

void ShellHost::ApplySafeAreaLayout() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  const float titlebar_dp = state_.titlebar_visible ? config_.titlebar_height_dp : 0.f;
  // Bottom chrome paints in a sibling of #shell-root. Keep the document
  // full-bleed and lift #shell-root by the panel height so nav stays above it
  // (same visual as OSK document-bottom inset).
  const int layout_bottom = bottom_chrome_open_ ? 0 : state_.safe_area_bottom_dp;
  // Only a keyboard-sized inset: the home-indicator inset is already part of the menus' own margins.
  ContextMenuHost::Instance().SetBottomInsetDp(state_.safe_area_bottom_dp >= kImeLatchMinDp ? state_.safe_area_bottom_dp : 0);
  const CompactChromeLayout layout = ShellLayout::ComputeCompactChromeLayout(
      config_, state_.safe_area_top_dp, layout_bottom, titlebar_dp);
  ui::ElementDocument* doc = context_->GetDocument(0);

  auto set_dp = [](ui::Element* element, const char* property, float value_dp) {
    if (!element) {
      return;
    }
    const std::string value = std::to_string(static_cast<int>(value_dp)) + "dp";
    element->SetProperty(property, value.c_str());
  };

  // Body stays full-bleed at the top so the theme background paints under the
  // translucent status bar. Content + chrome are inset via content_top_dp.
  set_dp(doc, "top", layout.shell_top_dp);
  set_dp(doc, "bottom", layout.shell_bottom_dp);
  set_dp(doc->GetElementById("shell-root"), "top", layout.content_top_dp);
  const float statusbar_dp = state_.statusbar_visible ? config_.statusbar_height_dp : 0.f;
  const float bottom_chrome_dp =
      bottom_chrome_open_ ? static_cast<float>(bottom_chrome_height_dp_) : 0.f;
  // Lift shell-root above the bottom chrome (nav chrome lives inside shell-root).
  set_dp(doc->GetElementById("shell-root"), "bottom", statusbar_dp + bottom_chrome_dp);
  set_dp(doc->GetElementById("shell-chrome"), "padding-top", layout.content_top_dp);
  // Absolute toast stack is positioned from the chrome top edge (ignores padding).
  set_dp(doc->GetElementById("shell-toast-stack"), "top", 8.f + layout.content_top_dp);

  if (state_.titlebar_visible) {
    DesktopWindowChrome::SetLayout(config_.titlebar_height_dp, config_.titlebar_controls_width_dp,
                                   5.f, state_.titlebar_traffic_lights);
  }

  if (ui::Element* panel = doc->GetElementById("shell-emoji-keyboard-panel")) {
    set_dp(panel, "height",
           bottom_chrome_dp > 0.f ? bottom_chrome_dp : static_cast<float>(kDefaultEmojiKeyboardPanelDp));
  }

  if (state_.layout_mode != LayoutMode::Compact) {
    return;
  }
  // Nav page only needs nav-height padding; shell-root bottom already clears the bottom chrome.
  set_dp(doc->GetElementById("shell-nav-page"), "padding-bottom", layout.content_padding_bottom_dp);
  set_dp(doc->GetElementById("shell-bottom-chrome"), "bottom", layout.chrome_bottom_dp);
  // Auxiliary sheet sits above the nav rail; account sheet covers it and draws from
  // the shell bottom (safe-area already applied via document bottom inset).
  set_dp(doc->GetElementById("shell-auxiliary-sheet"), "bottom", layout.sheet_bottom_dp);
  set_dp(doc->GetElementById("shell-account-sheet"), "bottom", layout.chrome_bottom_dp);
  // Chat overlay fills shell-root (already lifted); no extra bottom inset.
  set_dp(doc->GetElementById("shell-chat-overlay"), "bottom", 0.f);
  set_dp(doc->GetElementById("shell-chat-overlay"), "padding-bottom", 0.f);
}

bool ShellHost::ChromeFrostEnabled() const {
  return !state_.reduce_transparency && state_.compact_chrome_frost;
}

void ShellHost::SyncChromeMaterialPrefs(bool reduce_transparency, bool compact_chrome_frost) {
  if (state_.reduce_transparency == reduce_transparency &&
      state_.compact_chrome_frost == compact_chrome_frost) {
    return;
  }
  state_.reduce_transparency = reduce_transparency;
  state_.compact_chrome_frost = compact_chrome_frost;
  RequestSyncLayout();
}

ShellHost::ChromePrefs ShellHost::ProjectChrome(const ProfilePreferences& prefs) {
  return {.theme = prefs.theme,
          .appearance = prefs.appearance,
          .reduce_transparency = prefs.reduce_transparency,
          .compact_chrome_frost = prefs.compact_chrome_frost};
}

void ShellHost::Apply(const ChromePrefs& prefs) {
  SyncChromeMaterialPrefs(prefs.reduce_transparency, prefs.compact_chrome_frost);
}

void ShellHost::OnLayoutModeChanged() {
  statusbar_.RefreshVisibility();
  if (state_.layout_mode == LayoutMode::Expanded) {
    state_.compact_chat_open = false;
    if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::CompactChatOverlay) {
      pending_dismiss_.reset();
    }
    DetachDismissGestures();
  }
  if (on_layout_mode_changed_) {
    on_layout_mode_changed_(state_.layout_mode);
  }
}

ShellLayoutRml ShellHost::Rml() const {
  return ShellLayoutRml(state_, bottom_chrome_open_, bottom_chrome_key_);
}

const char* ShellHost::NavContentKey() const {
  return ShellLayout::NavContentKey(state_.nav_tab);
}

void ShellHost::MountNavRail() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::Element* target = context_->GetDocument(0)->GetElementById("shell-nav-rail-mount");
  if (!target) {
    return;
  }
  const std::string body = ViewCatalog::LoadBody("nav_rail");
  if (!body.empty()) {
    RmlMount::MountInner(target, body);
  }
}

void ShellHost::MountNavContent() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  const char* key = NavContentKey();
  if (!key) {
    return;
  }
  ui::Element* target = context_->GetDocument(0)->GetElementById(("pane-body-" + std::string(key)).c_str());
  if (!target) {
    return;
  }
  const std::string body = ViewCatalog::LoadBody(key);
  if (!body.empty()) {
    RmlMount::MountInner(target, body);
  }
}

void ShellHost::MountComposer() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  const std::string body = ViewCatalog::LoadBody("composer");
  if (body.empty()) {
    return;
  }

  // Home landing mounts the composer inside the home view (centered with chips).
  if (state_.nav_tab == NavTab::Home && !state_.compact_chat_open) {
    if (ui::Element* target = doc->GetElementById("home-composer-mount")) {
      RmlMount::MountInner(target, body);
    }
    return;
  }

  const PaneState* composer_pane = nullptr;
  if (state_.layout_mode == LayoutMode::Expanded) {
    if (!state_.primary_pane_key.empty()) {
      composer_pane = FindPane(state_.primary_pane_key.c_str());
    }
  } else {
    composer_pane = FindPane("chat");
  }
  if (!composer_pane || !composer_pane->spec.provides_composer) {
    return;
  }

  if (state_.layout_mode == LayoutMode::Expanded) {
    ui::Element* target = doc->GetElementById(("pane-composer-" + composer_pane->spec.key).c_str());
    if (target) {
      RmlMount::MountInner(target, body);
    }
    return;
  }

  if (!state_.compact_chat_open) {
    return;
  }
  ui::Element* target = doc->GetElementById("shell-composer-mount");
  if (target) {
    RmlMount::MountInner(target, body);
  }
}

void ShellHost::DetachDismissGestures() {
  swipe_back_gesture_.Detach();
  account_sheet_gesture_.Detach();
  gesture_axis_lock_.Unlock();
}

void ShellHost::AttachSwipeBackGesture() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  ShellSwipeBackGesture::AttachOptions options;
  options.width_dp_fallback = state_.shell_width_dp;
  options.dragging_class = "shell-swipe-dragging";
  options.axis_lock = &gesture_axis_lock_;
  options.require_edge = true;

  if (HasLocalBack("settings_detail")) {
    ui::Element* detail = doc->GetElementById("settings-detail-view");
    // Listen on the pane body (full sheet width) so the 12dp settings-panel padding
    // and true screen-left edge still arm swipe-back; slide only the detail view.
    ui::Element* listen = doc->GetElementById("pane-body-settings");
    if (!listen) {
      listen = doc->GetElementById("shell-account-sheet");
    }
    if (detail && listen) {
      options.chrome_classes = {"settings-detail-header", "settings-back-btn", "shell-back-btn"};
      options.transform_target = detail;
      options.content_root = detail;
      swipe_back_gesture_.Attach(listen, context_, std::move(options), [this]() {
        RequestDismiss(DismissStyle::Animated, DismissTarget::LocalBack);
      });
      return;
    }
  }

  if (!state_.transient_stack.empty()) {
    if (ui::Element* layer = doc->GetElementById("shell-transient-layer")) {
      options.chrome_classes = {"shell-transient-chrome", "shell-back-btn"};
      options.axis_lock = nullptr; // no sheet competition on transient
      swipe_back_gesture_.Attach(layer, context_, std::move(options),
                                 [this]() { RequestDismiss(DismissStyle::Animated, DismissTarget::Transient); });
      return;
    }
  }

  if (state_.layout_mode == LayoutMode::Compact && state_.compact_chat_open) {
    if (ui::Element* overlay = doc->GetElementById("shell-chat-overlay")) {
      options.chrome_classes = {"shell-chat-overlay-chrome", "shell-back-btn"};
      options.axis_lock = nullptr;
      swipe_back_gesture_.Attach(overlay, context_, std::move(options), [this]() {
        RequestDismiss(DismissStyle::Animated, DismissTarget::CompactChatOverlay);
      });
    }
  }
}

void ShellHost::AttachAccountSheetGesture() {
  if (!context_ || context_->GetNumDocuments() == 0 || !state_.account_sheet_open) {
    return;
  }
  ui::Element* sheet = context_->GetDocument(0)->GetElementById("shell-account-sheet");
  if (!sheet) {
    return;
  }
  // Fallback until layout resolves; gesture prefers the live box via ResolveSheetHeightDp.
  // Matches .shell-account-sheet top peek (toolbar_height_dp / 48dp).
  const float dp_ratio = context_->GetDensityIndependentPixelRatio();
  const float height_dp =
      (dp_ratio > 0.f) ? (static_cast<float>(context_->GetDimensions().y) / dp_ratio) : state_.shell_width_dp;
  const float sheet_height_dp = std::max(0.f, height_dp - config_.toolbar_height_dp);
  account_sheet_gesture_.Attach(
      sheet, context_, sheet_height_dp,
      [this]() { RequestDismiss(DismissStyle::Animated, DismissTarget::AccountSheet); }, &gesture_axis_lock_);
}

void ShellHost::MountPaneBodies() {
  auto mount_key = [this](const std::string& key) {
    if (!context_ || context_->GetNumDocuments() == 0) {
      return;
    }
    ui::ElementDocument* doc = context_->GetDocument(0);
    ui::Element* target = doc->GetElementById(("pane-body-" + key).c_str());
    if (!target) {
      return;
    }
    const std::string body = ViewCatalog::LoadBody(key);
    if (body.empty()) {
      // Avoid silent blank panes (common when packaged asset reads fail).
      ui::Log::Message(ui::Log::LT_ERROR, "ShellHost: failed to load view body for '%s'", key.c_str());
      return;
    }
    RmlMount::MountInner(target, body);
  };

  DetachDismissGestures();
  MountNavRail();
  MountNavContent();

  if (state_.layout_mode == LayoutMode::Expanded) {
    ui::String primary_key = state_.primary_pane_key;
    if (primary_key.empty() && state_.nav_tab == NavTab::Home) {
      primary_key = "home";
    }
    if (!primary_key.empty()) {
      mount_key(primary_key.c_str());
      if (primary_key == "home") {
        MountComposer();
      } else if (const PaneState* pane = FindPane(primary_key.c_str())) {
        if (pane->spec.provides_composer) {
          MountComposer();
        }
      }
    }
  } else if (state_.compact_chat_open) {
    if (state_.nav_tab == NavTab::Sessions) {
      mount_key("sidebar");
    }
    mount_key("chat");
    MountComposer();
  } else if (state_.nav_tab == NavTab::Home) {
    mount_key("home");
    MountComposer();
  }

  if (state_.auxiliary_open) {
    mount_key("preview");
  }

  if (state_.account_sheet_open) {
    mount_key("settings");
  }

  if (!state_.transient_stack.empty()) {
    const std::string& transient_key = state_.transient_stack.back().spec.key;
    if (on_before_transient_mount_) {
      on_before_transient_mount_(transient_key);
    }
    mount_key(transient_key);
    if (on_transient_mounted_) {
      on_transient_mounted_(transient_key);
    }
  }
  for (const OverlayEntry& overlay : state_.overlay_stack) {
    if (!context_ || context_->GetNumDocuments() == 0) {
      continue;
    }
    ui::Element* target =
        context_->GetDocument(0)->GetElementById(("overlay-body-" + std::to_string(overlay.id)).c_str());
    if (!target) {
      continue;
    }
    const std::string body = ViewCatalog::LoadBody(overlay.rml_path);
    if (!body.empty()) {
      RmlMount::MountInner(target, body);
    }
  }

  RemountBottomChromeNow();
  RefreshDismissGestures();
}

void ShellHost::RemountBottomChrome() {
  if (remount_bottom_chrome_pending_) {
    Backend::RequestForceFrame();
    return;
  }
  remount_bottom_chrome_pending_ = true;
  AppRuntime::PostUI([this]() { FlushRemountBottomChrome(); });
  Backend::RequestForceFrame();
}

void ShellHost::FlushRemountBottomChrome() {
  if (!remount_bottom_chrome_pending_) {
    return;
  }
  remount_bottom_chrome_pending_ = false;
  RemountBottomChromeNow();
}

void ShellHost::RemountBottomChromeNow() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  ui::Element* mount = doc->GetElementById("shell-emoji-keyboard-mount");
  if (!mount) {
    return;
  }
  const std::string html = Rml().BottomChrome();
  RmlMount::MountInner(mount, html);
  if (!html.empty()) {
    ui::Element* target = doc->GetElementById("shell-bottom-chrome-body");
    if (target && !bottom_chrome_rml_path_.empty()) {
      const std::string body = ViewCatalog::LoadBody(bottom_chrome_rml_path_);
      if (!body.empty()) {
        RmlMount::MountInner(target, body);
      }
    }
  }
  ApplySafeAreaLayout();
}

void ShellHost::SyncLayout() {
  // Taken before any early return, so a skipped flush cannot leave its reason for the next direct call.
  const std::string reason = std::exchange(running_sync_reason_, std::string("direct"));
  const int requests = std::exchange(running_sync_requests_, 1);
  ui::Element* root = ShellRoot();
  if (!root) {
    return;
  }
  // The whole shell DOM is rebuilt here, on the UI thread. Report the slow ones with who asked, so the
  // requests that did not need a full rebuild can be found (device logs: 75-230 ms each on a phone).
  const auto started = std::chrono::steady_clock::now();
  struct Report {
    std::chrono::steady_clock::time_point started;
    const std::string& reason;
    int requests;
    ~Report() {
      // Also runs while a failed rebuild unwinds: diagnostics must never turn that into a terminate.
      try {
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
        if (ms >= kSlowSyncLayoutMs) {
          MetricsLine("shell.sync_layout_slow")
              .Add("ms", static_cast<int64_t>(ms))
              .Add("reason", reason)
              .Add("requests", static_cast<int64_t>(requests))
              .Emit();
        }
      } catch (...) {
      }
    }
  } report{started, reason, requests};
  // Nestable remount gate: field blur/change commits must not run until settle.
  UiEditSession::Instance().BeginRemount();
  const LayoutMode mode = state_.layout_mode;
  splitter_drag_.Detach();
  aux_splitter_drag_.Detach();
  RmlMount::MountInner(root, Rml().ShellRoot());
  last_synced_mode_ = mode;
  MountPaneBodies();
  AttachSplitterDrag();
  remount_call_chrome_pending_ = false;
  remount_dialog_chrome_pending_ = false;
  remount_pin_gate_chrome_pending_ = false;
  RemountDialogChromeNow();
  RemountCallChromeNow();
  RemountPinGateChromeNow();
  RemountOverlayBodiesNow();
  DirtyWindow();
  if (state_.auxiliary_open) {
    DataModelHost::Instance().Dirty("shell", "working_set_active");
    DataModelHost::Instance().Dirty("shell", "working_set_title");
    DataModelHost::Instance().Dirty("shell", "working_set_subtitle");
    DataModelHost::Instance().Dirty("shell", "working_set_rml");
    DataModelHost::Instance().Dirty("shell", "working_set");
  }

  if (on_layout_synced_) {
    on_layout_synced_();
  }
  ApplySafeAreaLayout();
  // Spurious change/blur can land on the next UI turn after remount.
  AppRuntime::PostUI([]() { UiEditSession::Instance().EndRemount(); });
}

void ShellHost::AttachSplitterDrag() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  AttachAuxiliarySplitterDrag(doc);
  if (!ShellLayout::SecondaryPaneShown(state_)) {
    return;
  }
  ui::Element* handle = doc ? doc->GetElementById("shell-pane-splitter") : nullptr;
  ui::Element* pane = doc ? doc->GetElementById("shell-nav-content-mount") : nullptr;
  if (!handle || !pane) {
    return;
  }
  splitter_drag_.Attach(handle, pane, context_, state_.sidebar_width_dp, ShellSplitterDrag::Edge::Right,
                        ShellLayout::kSidebarMinWidthDp, ShellLayout::kSidebarMaxWidthDp, [this](int width_dp) {
                          state_.sidebar_width_dp = width_dp;
                          NotifySidebarChanged();
                        });
}

void ShellHost::AttachAuxiliarySplitterDrag(ui::ElementDocument* doc) {
  if (!doc || state_.layout_mode != LayoutMode::Expanded || !state_.auxiliary_open) {
    return;
  }
  ui::Element* handle = doc->GetElementById("shell-aux-splitter");
  ui::Element* pane = nullptr;
  for (const PaneState& candidate : state_.panes) {
    if (candidate.spec.role == PaneRole::Auxiliary) {
      pane = doc->GetElementById(ui::String("pane-") + candidate.spec.key);
    }
  }
  if (!handle || !pane) {
    return;
  }
  aux_splitter_drag_.Attach(handle, pane, context_, state_.auxiliary_width_dp, ShellSplitterDrag::Edge::Left,
                            ShellLayout::kAuxiliaryMinWidthDp, ShellLayout::kAuxiliaryMaxWidthDp,
                            [this](int width_dp) {
                              state_.auxiliary_width_dp = width_dp;
                              if (on_auxiliary_width_changed_) {
                                on_auxiliary_width_changed_(width_dp);
                              }
                            });
}

void ShellHost::RemountCallChrome() {
  // Never MountInner from inside an Rml click (Leave/Accept) — that destroys the event target
  // mid-dispatch (same class of crash as sync SyncLayout from a button).
  if (remount_call_chrome_pending_) {
    Backend::RequestForceFrame();
    return;
  }
  remount_call_chrome_pending_ = true;
  AppRuntime::PostUI([this]() { FlushRemountCallChrome(); });
  Backend::RequestForceFrame();
}

void ShellHost::FlushRemountCallChrome() {
  if (!remount_call_chrome_pending_) {
    return;
  }
  remount_call_chrome_pending_ = false;
  RemountCallChromeNow();
  // MountInner binds data views after any Dirty applied while remount was pending — dirty again
  // next UI turn so Connected/elapsed replace baked Connecting… text through the text bindings.
  AppRuntime::PostUI([this]() {
    DirtyCallChrome();
    Backend::RequestForceFrame();
  });
}

void ShellHost::DetachCallChromeGesture() {
  call_chrome_gesture_.Detach();
}

void ShellHost::AttachCallChromeGesture() {
  DetachCallChromeGesture();
  if (!context_ || context_->GetNumDocuments() == 0 || !state_.call_in_progress.active) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  ui::Element* root = doc->GetElementById("shell-call-chrome-root");
  if (!root && state_.call_in_progress.mode == CallChromeMode::Minimized) {
    root = doc->GetElementById("shell-call-minimized-chip");
  }
  if (!root) {
    return;
  }
  // Minimized: attach to the chip so drag/tap don't require the full-screen layer.
  if (state_.call_in_progress.mode == CallChromeMode::Minimized) {
    if (ui::Element* chip = doc->GetElementById("shell-call-minimized-chip")) {
      root = chip;
    }
  }
  ShellCallChromeGesture::Callbacks callbacks;
  callbacks.on_minimize = [this]() {
    if (call_actions_.minimize_chrome) {
      call_actions_.minimize_chrome();
    }
  };
  callbacks.on_immersive = [this]() {
    if (call_actions_.immersive_chrome) {
      call_actions_.immersive_chrome();
    }
  };
  callbacks.on_expand = [this]() {
    if (call_actions_.expand_chrome) {
      call_actions_.expand_chrome();
    }
  };
  callbacks.on_restore = [this]() {
    if (call_actions_.restore_chrome_from_minimized) {
      call_actions_.restore_chrome_from_minimized();
    }
  };
  callbacks.on_chip_corner = [this](int corner) {
    if (call_actions_.set_minimized_corner) {
      call_actions_.set_minimized_corner(corner);
    }
  };
  call_chrome_gesture_.Attach(root, context_, state_.call_in_progress.mode, std::move(callbacks),
                              &gesture_axis_lock_);
}

void ShellHost::RemountCallChromeNow() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  DetachCallChromeGesture();
  if (ui::Element* ring_mount = doc->GetElementById("shell-call-ring-mount")) {
    RmlMount::MountInner(ring_mount, CallRingRml(state_));
  }
  if (ui::Element* bar_mount = doc->GetElementById("shell-call-in-progress-mount")) {
    RmlMount::MountInner(bar_mount, CallInProgressRml(state_));
  }
  DirtyCallChrome();
  AttachCallChromeGesture();
}

void ShellHost::RemountDialogChrome() {
  if (remount_dialog_chrome_pending_) {
    Backend::RequestForceFrame();
    return;
  }
  remount_dialog_chrome_pending_ = true;
  AppRuntime::PostUI([this]() { FlushRemountDialogChrome(); });
  Backend::RequestForceFrame();
}

void ShellHost::FlushRemountDialogChrome() {
  if (!remount_dialog_chrome_pending_) {
    return;
  }
  remount_dialog_chrome_pending_ = false;
  RemountDialogChromeNow();
  AppRuntime::PostUI([this]() {
    DirtyFeedback();
    Backend::RequestForceFrame();
  });
}

void ShellHost::RemountDialogChromeNow() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  if (ui::Element* mount = doc->GetElementById("shell-dialog-mount")) {
    RmlMount::MountInner(mount, Rml().Dialog());
  }
  DirtyFeedback();
}

void ShellHost::MountOverlayBody(const std::string& mount_id, const std::string& body) {
  // Remembered so SyncLayout can put it back: the wizard mounted right after unlock was wiped by
  // the full shell rebuild that follows (session list load / language apply) — fresh install on
  // the iPhone went straight to an empty home screen (Kenneth 2026-10-13).
  if (body.empty()) {
    overlay_bodies_.erase(mount_id);
  } else {
    overlay_bodies_[mount_id] = body;
  }
  // Deferred like the PIN gate: a click handler may still be mid-dispatch inside the old overlay.
  AppRuntime::PostUI([this, mount_id, body]() {
    ShellHost& shell = *this;
    if (!shell.context_ || shell.context_->GetNumDocuments() == 0) {
      return;
    }
    ui::ElementDocument* doc = shell.context_->GetDocument(0);
    if (!doc) {
      return;
    }
    if (ui::Element* mount = doc->GetElementById(mount_id.c_str())) {
      RmlMount::MountInner(mount, body);
    }
    Backend::RequestForceFrame();
  });
  Backend::RequestForceFrame();
}

void ShellHost::RemountPinGateChrome() {
  if (remount_pin_gate_chrome_pending_) {
    Backend::RequestForceFrame();
    return;
  }
  remount_pin_gate_chrome_pending_ = true;
  AppRuntime::PostUI([this]() { FlushRemountPinGateChrome(); });
  Backend::RequestForceFrame();
}

void ShellHost::FlushRemountPinGateChrome() {
  if (!remount_pin_gate_chrome_pending_) {
    return;
  }
  remount_pin_gate_chrome_pending_ = false;
  RemountPinGateChromeNow();
  AppRuntime::PostUI([this]() {
    DirtyPinGate();
    Backend::RequestForceFrame();
  });
}

void ShellHost::RemountOverlayBodiesNow() {
  if (overlay_bodies_.empty() || !context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  for (const auto& [mount_id, body] : overlay_bodies_) {
    if (ui::Element* mount = doc->GetElementById(mount_id.c_str())) {
      RmlMount::MountInner(mount, body);
    }
  }
}

void ShellHost::RemountPinGateChromeNow() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  if (ui::Element* mount = doc->GetElementById("shell-pin-gate-mount")) {
    RmlMount::MountInner(mount, Rml().PinGate());
  }
  DirtyPinGate();
}

void ShellHost::Update(ui::Context* context) {
  // Wall clock: power-save WaitEventTimeout can sleep up to ~2s between idle frames, so a
  // fake +=16ms clock made Short toasts linger far too long on idle mobile screens.
  // Must match ShellFeedback::ShowToast's default clock (steady_clock).
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  elapsed_ms_ = ShellFeedback::NowMs();
  const size_t toast_count_before = state_.toasts.size();
  ShellFeedback::ExpireToasts(state_, elapsed_ms_);
  const bool banner_gone = ShellFeedback::ExpireBanner(state_, elapsed_ms_);
  if (state_.toasts.size() != toast_count_before || banner_gone) {
    DirtyFeedback();
  }

  if (pending_dismiss_ && now >= pending_dismiss_->at) {
    const DismissTarget target = pending_dismiss_->target;
    pending_dismiss_.reset();
    CommitDismiss(target);
  }

  const LayoutMode previous = state_.layout_mode;
  ApplyLayoutModeFromContext(context);
  RefreshSafeAreaInsets(context);
  if (previous != state_.layout_mode) {
    OnLayoutModeChanged();
    SaveFocus();
    SyncLayout();
    RestoreFocus();
    return;
  }
  statusbar_.RefreshCluster();
  if (state_.titlebar_visible) {
    const bool maximized = DesktopWindowChrome::IsMaximized();
    if (maximized != state_.window_maximized) {
      state_.window_maximized = maximized;
      DesktopWindowChrome::RefreshAppearance();
      statusbar_.Dirty();
    }
  }
}

void ShellHost::NotifyFrameEnd(ui::Context* context) {
  if (!context) {
    return;
  }

  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  double delay_sec = std::numeric_limits<double>::infinity();

  if (pending_dismiss_) {
    const double remaining = std::chrono::duration<double>(pending_dismiss_->at - now).count();
    delay_sec = std::min(delay_sec, std::max(0.0, remaining));
  }

  const double toast_delay = ShellFeedback::SecondsUntilNextFeedbackExpiry(state_, elapsed_ms_);
  if (toast_delay >= 0.0) {
    delay_sec = std::min(delay_sec, toast_delay);
  }

  // Call ring pulse / in-call meters need sub-second frames while overlays are active.
  if (state_.call_ring.active || state_.call_in_progress.active) {
    delay_sec = std::min(delay_sec, kCallChromeFrameIntervalSec);
  }

  if (std::isfinite(delay_sec)) {
    // Must run after Context::Update — that call resets next_update_timeout to infinity.
    context->RequestNextUpdate(delay_sec);
  }
}

void ShellHost::OnNavTabClicked(const std::string& tab_name) {
  const NavTab tab = NavTabFromString(tab_name.c_str());
  // A nav-rail click on a tab with a secondary pane (including the active tab) brings a collapsed list back.
  if (state_.layout_mode == LayoutMode::Expanded &&
      ShellLayout::SidebarCollapsedAfterNavSelect(state_.sidebar_collapsed, tab) != state_.sidebar_collapsed) {
    state_.sidebar_collapsed = false;
    NotifySidebarChanged();
    DirtyNavChrome();
    RequestSyncLayout();
  }
  SelectNavTab(tab);
}

void ShellHost::DismissBanner() {
  ShellFeedback::DismissBanner(state_);
  DirtyFeedback();
}

void ShellHost::DialogOk() {
  ShellFeedback::DialogOk(state_);
}

void ShellHost::DialogCancel() {
  ShellFeedback::DialogCancel(state_);
}

void ShellHost::DialogToggleCheckbox() {
  state_.dialog.checkbox_checked = !state_.dialog.checkbox_checked;
  DirtyFeedback();
}

void ShellHost::PinGateSubmit() {
  if (pin_gate_actions_.on_submit) {
    pin_gate_actions_.on_submit();
  }
}

void ShellHost::PinGateCancel() {
  if (pin_gate_actions_.on_cancel) {
    pin_gate_actions_.on_cancel();
  }
}

void ShellHost::PinGateSetPin() {
  if (pin_gate_actions_.on_set_pin) {
    pin_gate_actions_.on_set_pin();
  }
}

void ShellHost::PinGateUseDefault() {
  if (pin_gate_actions_.on_use_default) {
    pin_gate_actions_.on_use_default();
  }
}

void ShellHost::PinGateIdentityNew() {
  if (pin_gate_actions_.on_identity_new) {
    pin_gate_actions_.on_identity_new();
  }
}

void ShellHost::PinGateIdentityLink() {
  if (pin_gate_actions_.on_identity_link) {
    pin_gate_actions_.on_identity_link();
  }
}

void ShellHost::PinGateIdentityRestore() {
  if (pin_gate_actions_.on_identity_restore) {
    pin_gate_actions_.on_identity_restore();
  }
}

void ShellHost::PinGateForgotToggle() {
  if (pin_gate_actions_.on_forgot_toggle) {
    pin_gate_actions_.on_forgot_toggle();
  }
}

void ShellHost::CallAccept() {
  log().warning << "call_accept click";
  if (call_actions_.accept_incoming) {
    call_actions_.accept_incoming();
  }
}

void ShellHost::CallAcceptVoice() {
  log().warning << "call_accept_voice click";
  if (call_actions_.accept_incoming_voice_only) {
    call_actions_.accept_incoming_voice_only();
  }
}

void ShellHost::CallAcceptCharge() {
  log().warning << "call_accept_charge click";
  if (call_actions_.accept_incoming_with_charge) {
    call_actions_.accept_incoming_with_charge();
  }
}

void ShellHost::CallDecline() {
  if (call_actions_.decline_incoming) {
    call_actions_.decline_incoming();
  }
}

void ShellHost::CallLeave() {
  if (call_actions_.leave_active) {
    call_actions_.leave_active();
  }
}

void ShellHost::CallRetry() {
  if (call_actions_.retry_connect) {
    call_actions_.retry_connect();
  }
}

void ShellHost::CallMute() {
  if (call_actions_.toggle_mute) {
    call_actions_.toggle_mute();
  }
}

void ShellHost::CallCamera() {
  if (call_actions_.toggle_camera) {
    call_actions_.toggle_camera();
  }
}

void ShellHost::CallSpeaker() {
  if (call_actions_.toggle_speaker) {
    call_actions_.toggle_speaker();
  }
}

void ShellHost::CallInvite() {
  if (call_actions_.open_mid_call_invite_picker) {
    call_actions_.open_mid_call_invite_picker();
  }
}

void ShellHost::CallMinimize() {
  if (call_actions_.minimize_chrome) {
    call_actions_.minimize_chrome();
  }
}

void ShellHost::CallExpand() {
  if (call_actions_.expand_chrome) {
    call_actions_.expand_chrome();
  }
}

void ShellHost::CallImmersive() {
  if (call_actions_.immersive_chrome) {
    call_actions_.immersive_chrome();
  }
}

void ShellHost::CallRestore() {
  if (call_actions_.restore_chrome_from_minimized) {
    call_actions_.restore_chrome_from_minimized();
  }
}

void ShellHost::CallDetails() {
  if (call_actions_.show_call_details) {
    call_actions_.show_call_details();
  }
}

void ShellHost::TitlebarMinimize() {
  DesktopWindowChrome::Minimize();
}

void ShellHost::TitlebarToggleMaximize() {
  DesktopWindowChrome::ToggleMaximize();
  state_.window_maximized = DesktopWindowChrome::IsMaximized();
  statusbar_.Dirty();
}

void ShellHost::TitlebarClose() {
  DesktopWindowChrome::Close();
}

} // namespace pbr
