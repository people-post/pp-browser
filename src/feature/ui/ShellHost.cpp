#include "feature/ui/ShellHost.h"

#include "base/i18n/LocalizationService.h"
#include "base/platform/BrowserThread.h"
#include "base/platform/PlatformNavigation.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/RmlMount.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/FlowCoordinator.h"
#include "feature/ui/SettingsController.h"
#include "feature/ui/ShellInterruption.h"
#include "feature/ui/ShellLayout.h"
#include "base/ui/ViewCatalog.h"

#include "RmlUi_Backend.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <sstream>
#include <string>

#if RMLUI_SDL_VERSION_MAJOR >= 3
#include <SDL3/SDL.h>
#endif

namespace pbr {

namespace {

std::optional<int> EventArgAsInt(const Rml::VariantList& args, size_t index = 0) {
  if (args.size() <= index) {
    return std::nullopt;
  }
  const Rml::Variant& value = args[index];
  switch (value.GetType()) {
  case Rml::Variant::INT:
    return value.Get<int>();
  case Rml::Variant::INT64:
    return static_cast<int>(value.Get<int64_t>());
  case Rml::Variant::FLOAT:
    return static_cast<int>(value.Get<float>());
  case Rml::Variant::DOUBLE:
    return static_cast<int>(value.Get<double>());
  default:
    return std::nullopt;
  }
}

NavTab NavTabFromString(const Rml::String& value) {
  if (value == "contacts") {
    return NavTab::Contacts;
  }
  if (value == "sessions") {
    return NavTab::Sessions;
  }
  return NavTab::Home;
}

std::string SurfaceChromeClass(CompactChromeFrostSurface surface, CompactChromeFrostSurface active,
                               bool frost_enabled, bool solid) {
  std::string cls = "surface-chrome";
  if (frost_enabled && surface != CompactChromeFrostSurface::None && surface == active) {
    cls += " surface-chrome--frost";
  }
  if (solid) {
    cls += " surface-chrome--solid";
  }
  return cls;
}

} // namespace

ShellHost& ShellHost::Instance() {
  static ShellHost host;
  return host;
}

bool ShellHost::RegisterWindowModel(Rml::Context* context) {
  return DataModelHost::Instance().Register(context, "window", [](Rml::DataModelConstructor& ctor) {
    ShellHost& host = ShellHost::Instance();
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
    ctor.Bind("pin_gate_chooser_mode", &host.state_.pin_gate.chooser_mode);
    ctor.Bind("pin_gate_create_mode", &host.state_.pin_gate.create_mode);
    ctor.Bind("pin_gate_title", &host.state_.pin_gate.title);
    ctor.Bind("pin_gate_message", &host.state_.pin_gate.message);
    ctor.Bind("pin_gate_error", &host.state_.pin_gate.error);
    ctor.Bind("pin_gate_pin", &host.state_.pin_gate.pin);
    ctor.Bind("pin_gate_pin_confirm", &host.state_.pin_gate.pin_confirm);
    ctor.Bind("activity_visible", &host.state_.activity_visible);

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

    ctor.BindEventCallback("toggle_auxiliary", &ShellHost::ToggleAuxiliaryCallback);
    ctor.BindEventCallback("open_auxiliary", &ShellHost::OpenAuxiliaryCallback);
    ctor.BindEventCallback("select_nav_tab", &ShellHost::SelectNavTabCallback);
    ctor.BindEventCallback("compact_chat_back", &ShellHost::CompactChatBackCallback);
    ctor.BindEventCallback("open_account_sheet", &ShellHost::OpenAccountSheetCallback);
    ctor.BindEventCallback("close_account_sheet", &ShellHost::CloseAccountSheetCallback);
    ctor.BindEventCallback("transient_back", &ShellHost::PopTransientCallback);
    ctor.BindEventCallback("close_layer", &ShellHost::CloseLayerCallback);
    ctor.BindEventCallback("dismiss_banner", &ShellHost::DismissBannerCallback);
    ctor.BindEventCallback("dialog_ok", &ShellHost::DialogOkCallback);
    ctor.BindEventCallback("dialog_cancel", &ShellHost::DialogCancelCallback);
    ctor.BindEventCallback("dialog_toggle_checkbox", &ShellHost::DialogToggleCheckboxCallback);
    ctor.BindEventCallback("pin_gate_submit", &ShellHost::PinGateSubmitCallback);
    ctor.BindEventCallback("pin_gate_cancel", &ShellHost::PinGateCancelCallback);
    ctor.BindEventCallback("pin_gate_set_pin", &ShellHost::PinGateSetPinCallback);
    ctor.BindEventCallback("pin_gate_use_default", &ShellHost::PinGateUseDefaultCallback);
  });
}

void ShellHost::Initialize(Rml::Context* context) {
  context_ = context;
  state_ = {};
  state_.layout_mode = LayoutMode::Expanded;
  ShellLayout::SyncLayoutModeString(state_);
  ShellLayout::SyncNavTabString(state_);
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
  PlatformNavigation::SetDismissHandler([] { return Instance().HandleDismiss(); });
}

Rml::Element* ShellHost::ShellRoot() const {
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
  DirtyWindow();
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
    SettingsController::Instance().OnAccountSheetClosed();
  }
}

void ShellHost::SetPrimaryPane(const std::string& key) {
  state_.primary_pane_key = key.c_str();
  RequestSyncLayout();
  DirtyWindow();
}

void ShellHost::ClearPrimaryPane() {
  SetPrimaryPane("");
}

void ShellHost::SelectNavTab(NavTab tab) {
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
  DirtyWindow();
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
  if (state_.account_sheet_open) {
    return;
  }
  state_.account_sheet_open = true;
  if (pending_dismiss_ && pending_dismiss_->target == DismissTarget::AccountSheet) {
    pending_dismiss_.reset();
  }
  SettingsController::Instance().OnAccountSheetOpened();
  RequestSyncLayout();
  DirtyWindow();
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
  SettingsController::Instance().OnAccountSheetClosed();
  RequestSyncLayout();
  DirtyWindow();
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
  entry.rml_path = spec.rml_path.empty() ? ViewCatalog::ResolvePath(spec.key) : spec.rml_path;
  state_.overlay_stack.push_back(std::move(entry));
  SaveFocus();
  RequestSyncLayout();
  return state_.overlay_stack.back().id;
}

void ShellHost::CloseLayer(int layer_id) {
  if (state_.overlay_stack.empty()) {
    return;
  }
  const int closing_id = layer_id < 0 ? state_.overlay_stack.back().id : layer_id;
  FlowCoordinator::Instance().NotifyLayerClosing(closing_id);
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
    DirtyWindow();
    return;
  }
  case DismissTarget::CompactChatOverlay:
    CloseCompactChat();
    DirtyWindow();
    return;
  case DismissTarget::AuxiliarySheet:
    CloseAuxiliary();
    DirtyWindow();
    return;
  case DismissTarget::AccountSheet:
    CloseAccountSheet();
    return;
  case DismissTarget::Transient:
    PopTransient();
    DirtyWindow();
    return;
  case DismissTarget::OverlayLayer:
    CloseLayer();
    DirtyWindow();
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
  if (state_.dialog.active) {
    if (state_.dialog.show_cancel) {
      ShellFeedback::DialogCancel(state_);
    } else {
      ShellFeedback::DialogOk(state_);
    }
    RequestSyncLayout();
    DirtyWindow();
    return true;
  }
  if (state_.pin_gate.active) {
    if (state_.pin_gate.create_mode || state_.pin_gate.chooser_mode) {
      PinGateController::Instance().OnCancel();
    }
    // Unlock: consume Escape without dismissing or quitting.
    return true;
  }
  if (FlowCoordinator::Instance().HandleDismiss()) {
    RequestSyncLayout();
    DirtyWindow();
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

void ShellHost::DirtyWindow() {
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
  DataModelHost::Instance().Dirty("window", "auxiliary_available");
  DataModelHost::Instance().Dirty("window", "transient_active");
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
  DataModelHost::Instance().Dirty("window", "pin_gate_active");
  DataModelHost::Instance().Dirty("window", "pin_gate_chooser_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_create_mode");
  DataModelHost::Instance().Dirty("window", "pin_gate_title");
  DataModelHost::Instance().Dirty("window", "pin_gate_message");
  DataModelHost::Instance().Dirty("window", "pin_gate_error");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin");
  DataModelHost::Instance().Dirty("window", "pin_gate_pin_confirm");
  DataModelHost::Instance().Dirty("window", "activity_visible");
}

void ShellHost::RequestRemountNavRail() {
  BrowserThread::PostTask(BrowserThreadId::UI, []() {
    ShellHost& host = ShellHost::Instance();
    // Compact chat overlay omits the rail from the DOM.
    if (host.state_.layout_mode == LayoutMode::Compact && host.state_.compact_chat_open) {
      return;
    }
    host.MountNavRail();
    host.DirtyWindow();
  });
}

void ShellHost::SetActivityVisible(bool visible) {
  state_.activity_visible = visible;
  DirtyWindow();
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
  if (Rml::Element* focus = context_->GetFocusElement()) {
    saved_focus_id_ = focus->GetId();
  }
}

void ShellHost::RestoreFocus() {
  if (!context_ || saved_focus_id_.empty() || context_->GetNumDocuments() == 0) {
    saved_focus_id_.clear();
    return;
  }
  if (Rml::Element* element = context_->GetDocument(0)->GetElementById(saved_focus_id_)) {
    element->Focus();
  }
  saved_focus_id_.clear();
}

void ShellHost::RequestSyncLayout(bool restore_focus_after) {
  if (restore_focus_after) {
    restore_focus_after_sync_ = true;
  }
  if (sync_pending_) {
    return;
  }
  sync_pending_ = true;
  // Always defer: SyncLayout remounts the shell DOM. Flushing synchronously from a click
  // handler (e.g. compact_chat_back) destroys the target element mid-dispatch and crashes.
  BrowserThread::PostTask(BrowserThreadId::UI, []() { ShellHost::Instance().FlushPendingSyncLayout(); });
}

void ShellHost::FlushPendingSyncLayout() {
  if (!sync_pending_) {
    return;
  }
  sync_pending_ = false;
  SyncLayout();
  if (restore_focus_after_sync_) {
    restore_focus_after_sync_ = false;
    RestoreFocus();
  }
}

void ShellHost::ApplyLayoutModeFromContext(Rml::Context* context) {
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

void ShellHost::SetSafeAreaInsetsFromPrefs(int top_dp, int bottom_dp) {
  safe_area_top_from_prefs_dp_ = std::max(0, top_dp);
  safe_area_bottom_from_prefs_dp_ = std::max(0, bottom_dp);
  state_.safe_area_top_dp = std::max(state_.safe_area_top_dp, safe_area_top_from_prefs_dp_);
  state_.safe_area_bottom_dp = std::max(state_.safe_area_bottom_dp, safe_area_bottom_from_prefs_dp_);
  ApplySafeAreaLayout();
}

ShellHost::SafeAreaFromSdl ShellHost::ReadSafeAreaFromSdl() const {
  SafeAreaFromSdl insets{};
#if RMLUI_SDL_VERSION_MAJOR >= 3
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
  const int bottom_win = std::max(0, win_h - (safe.y + safe.h));
  insets.top_dp = static_cast<int>(static_cast<float>(top_win) * window_to_dp + 0.5f);
  insets.bottom_dp = static_cast<int>(static_cast<float>(bottom_win) * window_to_dp + 0.5f);
#endif
  return insets;
}

void ShellHost::RefreshSafeAreaInsets(Rml::Context* context) {
  (void)context;
  const SafeAreaFromSdl sdl = ReadSafeAreaFromSdl();
  const int effective_top = std::max(sdl.top_dp, safe_area_top_from_prefs_dp_);
  const int effective_bottom = std::max(sdl.bottom_dp, safe_area_bottom_from_prefs_dp_);
  if (effective_top == state_.safe_area_top_dp && effective_bottom == state_.safe_area_bottom_dp) {
    return;
  }
  state_.safe_area_top_dp = effective_top;
  state_.safe_area_bottom_dp = effective_bottom;
  ApplySafeAreaLayout();
}

void ShellHost::ApplySafeAreaLayout() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  const CompactChromeLayout layout = ShellLayout::ComputeCompactChromeLayout(
      config_, state_.safe_area_top_dp, state_.safe_area_bottom_dp);
  Rml::ElementDocument* doc = context_->GetDocument(0);

  auto set_dp = [](Rml::Element* element, const char* property, float value_dp) {
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
  set_dp(doc->GetElementById("shell-chrome"), "padding-top", layout.content_top_dp);
  // Absolute toast stack is positioned from the chrome top edge (ignores padding).
  set_dp(doc->GetElementById("shell-toast-stack"), "top", 8.f + layout.content_top_dp);

  if (state_.layout_mode != LayoutMode::Compact) {
    return;
  }
  set_dp(doc->GetElementById("shell-nav-page"), "padding-bottom", layout.content_padding_bottom_dp);
  set_dp(doc->GetElementById("shell-bottom-chrome"), "bottom", layout.chrome_bottom_dp);
  // Auxiliary sheet sits above the nav rail; account sheet covers it and draws from
  // the shell bottom (safe-area already applied via document bottom inset).
  set_dp(doc->GetElementById("shell-auxiliary-sheet"), "bottom", layout.sheet_bottom_dp);
  set_dp(doc->GetElementById("shell-account-sheet"), "bottom", layout.chrome_bottom_dp);
  // Chat overlay hides the nav rail — no extra bottom padding beyond shell inset.
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

void ShellHost::OnLayoutModeChanged() {
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

const char* ShellHost::NavContentKey() const {
  return ShellLayout::NavContentKey(state_.nav_tab);
}

std::string ShellHost::SerializePaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot) const {
  std::ostringstream out;
  out << "<div class=\"shell-pane shell-pane-" << key;
  if (extra_class && extra_class[0] != '\0') {
    out << ' ' << extra_class;
  }
  out << "\" id=\"pane-" << key << "\">";
  out << "<div class=\"shell-pane-body\" id=\"pane-body-" << key << "\"></div>";
  if (with_composer_slot) {
    out << "<div class=\"shell-pane-composer\" id=\"pane-composer-" << key << "\"></div>";
  }
  out << "</div>";
  return out.str();
}

std::string ShellHost::SerializeExpandedBase() const {
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base\" data-model=\"window\">";
  out << "<div class=\"shell-pane-row\">";
  out << "<div class=\"shell-nav-rail\" id=\"shell-nav-rail-mount\"></div>";
  if (ShellLayout::TabHasSecondary(state_.nav_tab)) {
    const char* nav_content = ShellLayout::NavContentKey(state_.nav_tab);
    out << "<div class=\"shell-pane shell-pane-secondary\" id=\"shell-nav-content-mount\">";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-" << nav_content << "\"></div>";
    out << "</div>";
  }
  Rml::String primary_key = state_.primary_pane_key;
  if (primary_key.empty() && state_.nav_tab == NavTab::Home) {
    primary_key = "home";
  }
  if (!primary_key.empty()) {
    if (const PaneState* pane = FindPane(primary_key.c_str())) {
      out << SerializePaneSlot(pane->spec.key, "shell-pane-primary", pane->spec.provides_composer);
    }
  }
  if (state_.auxiliary_open) {
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Auxiliary) {
        out << SerializePaneSlot(pane.spec.key, "shell-pane-auxiliary");
      }
    }
  }
  out << "</div></div>";
  out << SerializeAccountSheet();
  return out.str();
}

std::string ShellHost::SerializeAccountSheet() const {
  if (!state_.account_sheet_open) {
    return {};
  }
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-account-sheet-scrim\" data-event-click=\"close_account_sheet()\"></div>";
  out << "<div class=\"shell-account-sheet surface-chrome";
  if (solid) {
    out << " surface-chrome--solid";
  }
  out << "\" id=\"shell-account-sheet\">";
  out << "<div class=\"shell-account-sheet-grabber\"></div>";
  out << "<div class=\"shell-account-sheet-header row "
      << SurfaceChromeClass(CompactChromeFrostSurface::AccountSheetHeader, frost, frost_enabled, solid) << "\">";
  out << "<h2 class=\"heading-2\">Me</h2>";
  out << "<button class=\"shell-close-btn\" type=\"button\" data-event-click=\"close_account_sheet()\">";
  out << "<svg src=\"../icons/close.svg\" width=\"14\" height=\"14\" crop-to-content=\"true\"></svg>";
  out << "</button>";
  out << "</div>";
  out << "<div class=\"shell-pane-body shell-account-sheet-body\" id=\"pane-body-settings\"></div>";
  out << "</div>";
  return out.str();
}

std::string ShellHost::SerializeCompactBase() const {
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base shell-layer-compact\" data-model=\"window\">";

  if (!state_.compact_chat_open) {
    const bool home_inline = state_.nav_tab == NavTab::Home;
    out << "<div class=\"shell-nav-page";
    if (home_inline) {
      out << " shell-nav-page--home";
    }
    out << "\" id=\"shell-nav-page\">";
    if (const char* nav_content = NavContentKey()) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-" << nav_content << "\"></div>";
    } else if (home_inline) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-home\"></div>";
    }
    out << "</div>";
  } else if (state_.nav_tab == NavTab::Sessions) {
    out << "<div class=\"shell-nav-page shell-nav-page--under-overlay\" id=\"shell-nav-page\">";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-sidebar\"></div>";
    out << "</div>";
  }

  if (state_.compact_chat_open) {
    out << "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">";
    out << "<div class=\"shell-chat-overlay-chrome row "
        << SurfaceChromeClass(CompactChromeFrostSurface::ChatOverlayHeader, frost, frost_enabled, solid) << "\">";
    out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"compact_chat_back()\">";
    out << "<svg src=\"../icons/back.svg\"></svg>";
    out << "</button>";
    out << "</div>";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-chat\"></div>";
    out << "<div class=\"shell-composer-mount surface-chrome";
    if (solid) {
      out << " surface-chrome--solid";
    }
    out << "\" id=\"shell-composer-mount\"></div>";
    out << "</div>";
  }

  if (state_.auxiliary_open) {
    out << "<div class=\"shell-sheet-scrim\" data-event-click=\"toggle_auxiliary()\"></div>";
    out << "<div class=\"shell-sheet shell-sheet-auxiliary shell-sheet-compact surface-chrome";
    if (solid) {
      out << " surface-chrome--solid";
    }
    out << "\" id=\"shell-auxiliary-sheet\">";
    out << "<div class=\"shell-sheet-compact-chrome "
        << SurfaceChromeClass(CompactChromeFrostSurface::AuxiliarySheetChrome, frost, frost_enabled, solid)
        << "\"></div>";
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Auxiliary) {
        out << SerializePaneSlot(pane.spec.key, nullptr);
      }
    }
    out << "</div>";
  }

  if (!state_.compact_chat_open) {
    out << "<div class=\"shell-bottom-chrome";
    if (frost == CompactChromeFrostSurface::BottomNav && frost_enabled) {
      out << " shell-bottom-chrome--frost";
    }
    if (solid) {
      out << " shell-bottom-chrome--solid";
    }
    out << "\" id=\"shell-bottom-chrome\">";
    out << "<div class=\"shell-nav-rail shell-nav-rail--compact\" id=\"shell-nav-rail-mount\"></div>";
    out << "</div>";
  }

  out << SerializeAccountSheet();
  out << "</div>";
  return out.str();
}

std::string ShellHost::SerializeTransientLayer() const {
  if (state_.transient_stack.empty()) {
    return {};
  }
  const PaneState& top = state_.transient_stack.back();
  const CompactChromeFrostSurface frost = ShellInterruption::ResolveFrostSurface(state_);
  const bool frost_enabled = ChromeFrostEnabled();
  const bool solid = state_.reduce_transparency;
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-transient\" id=\"shell-transient-layer\" data-model=\"window\">";
  out << "<div class=\"shell-transient-chrome "
      << SurfaceChromeClass(CompactChromeFrostSurface::TransientHeader, frost, frost_enabled, solid) << "\">";
  out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"transient_back()\">"
         "<svg src=\"../icons/back.svg\"></svg>"
         "</button>";
  out << "</div>";
  out << SerializePaneSlot(top.spec.key, "shell-pane-transient");
  out << "</div>";
  return out.str();
}

std::string ShellHost::SerializeOverlays() const {
  std::ostringstream out;
  for (const OverlayEntry& overlay : state_.overlay_stack) {
    out << "<div class=\"shell-layer shell-layer-overlay\" data-model=\"window\">";
    out << "<div class=\"shell-scrim\" data-event-click=\"close_layer(" << overlay.id << ")\"></div>";
    out << "<div class=\"shell-frame\">";
    out << "<button class=\"shell-close-btn\" data-event-click=\"close_layer(" << overlay.id << ")\">×</button>";
    out << "<div class=\"shell-overlay-body\" id=\"overlay-body-" << overlay.id << "\"></div>";
    out << "</div></div>";
  }
  return out.str();
}

std::string ShellHost::SerializeDialog() const {
  if (!state_.dialog.active) {
    return {};
  }
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-dialog\" data-model=\"window\">";
  if (state_.dialog.show_cancel) {
    out << "<div class=\"shell-scrim\" data-event-click=\"dialog_cancel()\"></div>";
  } else {
    out << "<div class=\"shell-scrim\" data-event-click=\"dialog_ok()\"></div>";
  }
  out << "<div class=\"shell-dialog\">";
  out << "<h2 class=\"heading-2 shell-dialog-title\" data-rml=\"dialog_title\"></h2>";
  out << "<p class=\"text shell-dialog-message\" data-rml=\"dialog_message\"></p>";
  out << "<label class=\"shell-dialog-checkbox row\" data-if=\"dialog_show_checkbox\">";
  out << "<input type=\"checkbox\" data-value=\"dialog_checkbox_checked\" "
         "data-event-change=\"dialog_toggle_checkbox()\"/>";
  out << "<span data-rml=\"dialog_checkbox_label\"></span>";
  out << "</label>";
  out << "<input class=\"shell-dialog-prompt\" type=\"text\" data-if=\"dialog_show_prompt\" "
         "data-value=\"dialog_prompt_value\"/>";
  out << "<div class=\"shell-dialog-actions row\">";
  out << "<button class=\"shell-dialog-cancel\" data-if=\"dialog_show_cancel\" "
         "data-event-click=\"dialog_cancel()\">"
      << Tr("common.cancel") << "</button>";
  out << "<button class=\"shell-dialog-ok\" data-event-click=\"dialog_ok()\">" << Tr("common.ok")
      << "</button>";
  out << "</div></div></div>";
  return out.str();
}

std::string ShellHost::SerializePinGate() const {
  if (!state_.pin_gate.active) {
    return {};
  }
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-dialog\" data-model=\"window\">";
  out << "<div class=\"shell-scrim\"></div>";
  out << "<div class=\"shell-dialog shell-pin-gate\">";
  out << "<h2 class=\"heading-2 shell-dialog-title\" data-rml=\"pin_gate_title\"></h2>";
  out << "<p class=\"text shell-dialog-message\" data-rml=\"pin_gate_message\"></p>";
  out << "<p class=\"text shell-pin-gate-error\" data-rml=\"pin_gate_error\"></p>";
  out << "<div class=\"shell-pin-gate-chooser\" data-if=\"pin_gate_chooser_mode\">";
  out << "<button class=\"shell-dialog-ok\" data-event-click=\"pin_gate_set_pin()\">" << Tr("pin.set_pin")
      << "</button>";
  out << "<button class=\"btn btn-secondary\" data-event-click=\"pin_gate_use_default()\">"
      << Tr("pin.just_continue") << "</button>";
  out << "<button class=\"shell-dialog-cancel\" data-event-click=\"pin_gate_cancel()\">" << Tr("pin.not_now")
      << "</button>";
  out << "</div>";
  out << "<input class=\"field shell-pin-gate-input\" type=\"password\" data-if=\"!pin_gate_chooser_mode\" "
         "data-value=\"pin_gate_pin\" placeholder=\""
      << Tr("pin.placeholder") << "\"/>";
  out << "<input class=\"field shell-pin-gate-input\" type=\"password\" "
         "data-if=\"pin_gate_create_mode && !pin_gate_chooser_mode\" data-value=\"pin_gate_pin_confirm\" "
         "placeholder=\""
      << Tr("pin.confirm_placeholder") << "\"/>";
  out << "<div class=\"shell-dialog-actions row\" data-if=\"!pin_gate_chooser_mode\">";
  out << "<button class=\"shell-dialog-cancel\" data-if=\"pin_gate_create_mode\" "
         "data-event-click=\"pin_gate_cancel()\">"
      << Tr("pin.not_now") << "</button>";
  out << "<button class=\"shell-dialog-ok\" data-event-click=\"pin_gate_submit()\">" << Tr("common.continue")
      << "</button>";
  out << "</div></div></div>";
  return out.str();
}

std::string ShellHost::SerializeShellRoot() const {
  std::ostringstream out;
  if (state_.layout_mode == LayoutMode::Expanded) {
    out << SerializeExpandedBase();
  } else {
    out << SerializeCompactBase();
  }
  out << SerializeTransientLayer();
  out << SerializeOverlays();
  out << SerializeDialog();
  out << SerializePinGate();
  return out.str();
}

void ShellHost::MountNavRail() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  Rml::Element* target = context_->GetDocument(0)->GetElementById("shell-nav-rail-mount");
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
  Rml::Element* target = context_->GetDocument(0)->GetElementById(("pane-body-" + std::string(key)).c_str());
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
  Rml::ElementDocument* doc = context_->GetDocument(0);
  const std::string body = ViewCatalog::LoadBody("composer");
  if (body.empty()) {
    return;
  }

  // Home landing mounts the composer inside the home view (centered with chips).
  if (state_.nav_tab == NavTab::Home && !state_.compact_chat_open) {
    if (Rml::Element* target = doc->GetElementById("home-composer-mount")) {
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
    Rml::Element* target = doc->GetElementById(("pane-composer-" + composer_pane->spec.key).c_str());
    if (target) {
      RmlMount::MountInner(target, body);
    }
    return;
  }

  if (!state_.compact_chat_open) {
    return;
  }
  Rml::Element* target = doc->GetElementById("shell-composer-mount");
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
  Rml::ElementDocument* doc = context_->GetDocument(0);
  ShellSwipeBackGesture::AttachOptions options;
  options.width_dp_fallback = state_.shell_width_dp;
  options.dragging_class = "shell-swipe-dragging";
  options.axis_lock = &gesture_axis_lock_;
  options.require_edge = true;

  if (HasLocalBack("settings_detail")) {
    Rml::Element* detail = doc->GetElementById("settings-detail-view");
    // Listen on the pane body (full sheet width) so the 12dp settings-panel padding
    // and true screen-left edge still arm swipe-back; slide only the detail view.
    Rml::Element* listen = doc->GetElementById("pane-body-settings");
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
    if (Rml::Element* layer = doc->GetElementById("shell-transient-layer")) {
      options.chrome_classes = {"shell-transient-chrome", "shell-back-btn"};
      options.axis_lock = nullptr; // no sheet competition on transient
      swipe_back_gesture_.Attach(layer, context_, std::move(options),
                                 [this]() { RequestDismiss(DismissStyle::Animated, DismissTarget::Transient); });
      return;
    }
  }

  if (state_.layout_mode == LayoutMode::Compact && state_.compact_chat_open) {
    if (Rml::Element* overlay = doc->GetElementById("shell-chat-overlay")) {
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
  Rml::Element* sheet = context_->GetDocument(0)->GetElementById("shell-account-sheet");
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
    Rml::ElementDocument* doc = context_->GetDocument(0);
    Rml::Element* target = doc->GetElementById(("pane-body-" + key).c_str());
    if (!target) {
      return;
    }
    const std::string body = ViewCatalog::LoadBody(key);
    if (body.empty()) {
      // Avoid silent blank panes (common when packaged asset reads fail).
      Rml::Log::Message(Rml::Log::LT_ERROR, "ShellHost: failed to load view body for '%s'", key.c_str());
      return;
    }
    RmlMount::MountInner(target, body);
  };

  DetachDismissGestures();
  MountNavRail();
  MountNavContent();

  if (state_.layout_mode == LayoutMode::Expanded) {
    Rml::String primary_key = state_.primary_pane_key;
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
    Rml::Element* target =
        context_->GetDocument(0)->GetElementById(("overlay-body-" + std::to_string(overlay.id)).c_str());
    if (!target) {
      continue;
    }
    const std::string body = ViewCatalog::LoadBody(overlay.rml_path);
    if (!body.empty()) {
      RmlMount::MountInner(target, body);
    }
  }

  RefreshDismissGestures();
}

void ShellHost::SyncLayout() {
  Rml::Element* root = ShellRoot();
  if (!root) {
    return;
  }
  const LayoutMode mode = state_.layout_mode;
  RmlMount::MountInner(root, SerializeShellRoot());
  last_synced_mode_ = mode;
  MountPaneBodies();
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
}

void ShellHost::Update(Rml::Context* context) {
  // Wall clock: power-save WaitEventTimeout can sleep up to ~2s between idle frames, so a
  // fake +=16ms clock made Short toasts linger far too long on idle mobile screens.
  // Must match ShellFeedback::ShowToast's default clock (steady_clock).
  using clock = std::chrono::steady_clock;
  const auto now = clock::now();
  elapsed_ms_ = static_cast<float>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
  const size_t toast_count_before = state_.toasts.size();
  ShellFeedback::ExpireToasts(state_, elapsed_ms_);
  if (state_.toasts.size() != toast_count_before) {
    DirtyWindow();
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
  DirtyWindow();
}

void ShellHost::NotifyFrameEnd(Rml::Context* context) {
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

  const double toast_delay = ShellFeedback::SecondsUntilNextToastExpiry(state_, elapsed_ms_);
  if (toast_delay >= 0.0) {
    delay_sec = std::min(delay_sec, toast_delay);
  }

  if (std::isfinite(delay_sec)) {
    // Must run after Context::Update — that call resets next_update_timeout to infinity.
    context->RequestNextUpdate(delay_sec);
  }
}

void ShellHost::ToggleAuxiliaryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().ToggleAuxiliary();
}

void ShellHost::OpenAuxiliaryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().OpenAuxiliary();
}

void ShellHost::SelectNavTabCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().SelectNavTab(NavTabFromString(args[0].Get<Rml::String>()));
}

void ShellHost::CompactChatBackCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& /*args*/) {
  Instance().RequestDismiss(DismissStyle::Instant, DismissTarget::CompactChatOverlay);
}

void ShellHost::OpenAccountSheetCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                         const Rml::VariantList& /*args*/) {
  Instance().OpenAccountSheet();
}

void ShellHost::CloseAccountSheetCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  // × always closes the sheet, even when a nested settings detail is open.
  Instance().CloseAccountSheet();
}

void ShellHost::PopTransientCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& /*args*/) {
  Instance().RequestDismiss(DismissStyle::Instant, DismissTarget::Transient);
}

void ShellHost::CloseLayerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                   const Rml::VariantList& args) {
  const int layer_id = EventArgAsInt(args).value_or(-1);
  Instance().CloseLayer(layer_id);
}

void ShellHost::DismissBannerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  ShellFeedback::DismissBanner(Instance().state_);
  Instance().DirtyWindow();
}

void ShellHost::DialogOkCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                 const Rml::VariantList& /*args*/) {
  ShellHost& host = Instance();
  ShellFeedback::DialogOk(host.state_);
  host.RequestSyncLayout();
  host.DirtyWindow();
}

void ShellHost::DialogCancelCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& /*args*/) {
  ShellHost& host = Instance();
  ShellFeedback::DialogCancel(host.state_);
  host.RequestSyncLayout();
  host.DirtyWindow();
}

void ShellHost::DialogToggleCheckboxCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  ShellHost& host = Instance();
  host.state_.dialog.checkbox_checked = !host.state_.dialog.checkbox_checked;
  host.DirtyWindow();
}

void ShellHost::PinGateSubmitCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  PinGateController::Instance().OnSubmit();
}

void ShellHost::PinGateCancelCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  PinGateController::Instance().OnCancel();
}

void ShellHost::PinGateSetPinCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  PinGateController::Instance().OnSetPin();
}

void ShellHost::PinGateUseDefaultCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                          const Rml::VariantList& /*args*/) {
  PinGateController::Instance().OnUseDefaultPin();
}

} // namespace pbr
