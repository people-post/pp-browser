#include "feature/ui/ShellHost.h"

#include "base/platform/BrowserThread.h"
#include "base/platform/PlatformNavigation.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/RmlMount.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellInterruption.h"
#include "feature/ui/ShellLayout.h"
#include "base/ui/ViewCatalog.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <sstream>
#include <string>

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
  if (value == "me" || value == "settings") {
    return NavTab::Me;
  }
  if (value == "contacts") {
    return NavTab::Contacts;
  }
  if (value == "sessions") {
    return NavTab::Sessions;
  }
  return NavTab::Home;
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
    ctor.Bind("activity_visible", &host.state_.activity_visible);

    ctor.BindEventCallback("toggle_auxiliary", &ShellHost::ToggleAuxiliaryCallback);
    ctor.BindEventCallback("open_auxiliary", &ShellHost::OpenAuxiliaryCallback);
    ctor.BindEventCallback("select_nav_tab", &ShellHost::SelectNavTabCallback);
    ctor.BindEventCallback("compact_chat_back", &ShellHost::CompactChatBackCallback);
    ctor.BindEventCallback("transient_back", &ShellHost::PopTransientCallback);
    ctor.BindEventCallback("close_layer", &ShellHost::CloseLayerCallback);
    ctor.BindEventCallback("dismiss_banner", &ShellHost::DismissBannerCallback);
    ctor.BindEventCallback("dialog_ok", &ShellHost::DialogOkCallback);
    ctor.BindEventCallback("dialog_cancel", &ShellHost::DialogCancelCallback);
    ctor.BindEventCallback("dialog_toggle_checkbox", &ShellHost::DialogToggleCheckboxCallback);
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
  compact_chat_dismiss_at_ms_ = -1.f;
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
  compact_chat_dismiss_at_ms_ = -1.f;
  DetachChatOverlayGesture();
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
  compact_chat_dismiss_at_ms_ = -1.f;
  RequestSyncLayout();
}

void ShellHost::CloseCompactChat() {
  if (!state_.compact_chat_open) {
    return;
  }
  state_.compact_chat_open = false;
  compact_chat_dismiss_at_ms_ = -1.f;
  DetachChatOverlayGesture();
  RequestSyncLayout();
}

void ShellHost::ScheduleCompactChatDismiss() {
  compact_chat_dismiss_at_ms_ = elapsed_ms_ + 220.f;
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
  state_.transient_stack.pop_back();
  state_.transient_active = !state_.transient_stack.empty();
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
  if (!ShellInterruption::DismissTop(state_)) {
    return false;
  }
  DetachChatOverlayGesture();
  RequestSyncLayout();
  DirtyWindow();
  return true;
}

void ShellHost::DirtyWindow() {
  DataModelHost::Instance().Dirty("window", "layout_mode");
  DataModelHost::Instance().Dirty("window", "nav_tab");
  DataModelHost::Instance().Dirty("window", "compact_chat_open");
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
  DataModelHost::Instance().Dirty("window", "activity_visible");
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

void ShellHost::OnLayoutModeChanged() {
  if (state_.layout_mode == LayoutMode::Expanded) {
    state_.compact_chat_open = false;
    compact_chat_dismiss_at_ms_ = -1.f;
    DetachChatOverlayGesture();
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
    primary_key = "chat";
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
  return out.str();
}

std::string ShellHost::SerializeCompactBase() const {
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-base shell-layer-compact\" data-model=\"window\">";

  if (!state_.compact_chat_open) {
    const bool home_inline = state_.nav_tab == NavTab::Home;
    out << "<div class=\"shell-nav-page";
    if (home_inline) {
      out << " shell-nav-page--home";
    }
    out << "\">";
    if (const char* nav_content = NavContentKey()) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-" << nav_content << "\"></div>";
    } else if (home_inline) {
      out << "<div class=\"shell-pane-body\" id=\"pane-body-chat\"></div>";
      out << "<div class=\"shell-composer-mount\" id=\"shell-composer-mount\"></div>";
    }
    out << "</div>";
  } else if (state_.nav_tab == NavTab::Sessions) {
    out << "<div class=\"shell-nav-page shell-nav-page--under-overlay\">";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-sidebar\"></div>";
    out << "</div>";
  }

  if (state_.compact_chat_open) {
    out << "<div class=\"shell-chat-overlay\" id=\"shell-chat-overlay\">";
    out << "<div class=\"shell-chat-overlay-chrome row\">";
    out << "<button class=\"shell-back-btn\" type=\"button\" data-event-click=\"compact_chat_back()\">";
    out << "<svg src=\"../icons/back.svg\"></svg>";
    out << "</button>";
    out << "</div>";
    out << "<div class=\"shell-pane-body\" id=\"pane-body-chat\"></div>";
    out << "<div class=\"shell-composer-mount\" id=\"shell-composer-mount\"></div>";
    out << "</div>";
  }

  if (state_.auxiliary_open) {
    out << "<div class=\"shell-sheet-scrim\" data-event-click=\"toggle_auxiliary()\"></div>";
    out << "<div class=\"shell-sheet shell-sheet-auxiliary shell-sheet-compact\">";
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Auxiliary) {
        out << SerializePaneSlot(pane.spec.key, nullptr);
      }
    }
    out << "</div>";
  }

  if (!state_.compact_chat_open) {
    out << "<div class=\"shell-bottom-chrome\">";
    out << "<div class=\"shell-nav-rail shell-nav-rail--compact\" id=\"shell-nav-rail-mount\"></div>";
    out << "</div>";
  }

  out << "</div>";
  return out.str();
}

std::string ShellHost::SerializeTransientLayer() const {
  if (state_.transient_stack.empty()) {
    return {};
  }
  const PaneState& top = state_.transient_stack.back();
  std::ostringstream out;
  out << "<div class=\"shell-layer shell-layer-transient\" data-model=\"window\">";
  out << "<div class=\"shell-transient-chrome\">";
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
  out << "<div class=\"shell-dialog-actions row\">";
  out << "<button class=\"shell-dialog-cancel\" data-if=\"dialog_show_cancel\" "
         "data-event-click=\"dialog_cancel()\">Cancel</button>";
  out << "<button class=\"shell-dialog-ok\" data-event-click=\"dialog_ok()\">OK</button>";
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

  Rml::ElementDocument* doc = context_->GetDocument(0);
  const std::string body = ViewCatalog::LoadBody("composer");
  if (body.empty()) {
    return;
  }

  if (state_.layout_mode == LayoutMode::Expanded) {
    Rml::Element* target = doc->GetElementById(("pane-composer-" + composer_pane->spec.key).c_str());
    if (target) {
      RmlMount::MountInner(target, body);
    }
    return;
  }

  if (!state_.compact_chat_open && state_.nav_tab != NavTab::Home) {
    return;
  }
  Rml::Element* target = doc->GetElementById("shell-composer-mount");
  if (target) {
    RmlMount::MountInner(target, body);
  }
}

void ShellHost::AttachChatOverlayGesture() {
  if (!context_ || context_->GetNumDocuments() == 0 || state_.layout_mode != LayoutMode::Compact ||
      !state_.compact_chat_open) {
    DetachChatOverlayGesture();
    return;
  }
  Rml::Element* overlay = context_->GetDocument(0)->GetElementById("shell-chat-overlay");
  if (!overlay) {
    return;
  }
  chat_overlay_gesture_.Attach(overlay, context_, state_.shell_width_dp, [this]() { ScheduleCompactChatDismiss(); });
}

void ShellHost::DetachChatOverlayGesture() {
  chat_overlay_gesture_.Detach();
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
    if (!body.empty()) {
      RmlMount::MountInner(target, body);
    }
  };

  DetachChatOverlayGesture();
  MountNavRail();
  MountNavContent();

  if (state_.layout_mode == LayoutMode::Expanded) {
    Rml::String primary_key = state_.primary_pane_key;
    if (primary_key.empty() && state_.nav_tab == NavTab::Home) {
      primary_key = "chat";
    }
    if (!primary_key.empty()) {
      mount_key(primary_key.c_str());
      if (const PaneState* pane = FindPane(primary_key.c_str())) {
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
    AttachChatOverlayGesture();
  } else if (state_.nav_tab == NavTab::Home) {
    mount_key("chat");
    MountComposer();
  }

  if (state_.auxiliary_open) {
    mount_key("preview");
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
}

void ShellHost::Update(Rml::Context* context) {
  // Wall clock: power-save WaitEventTimeout can sleep up to ~10s between frames, so a
  // fake +=16ms clock made Short toasts linger for minutes on idle mobile screens.
  // Must match ShellFeedback::ShowToast's default clock (steady_clock).
  using clock = std::chrono::steady_clock;
  elapsed_ms_ = static_cast<float>(
      std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count());
  const size_t toast_count_before = state_.toasts.size();
  ShellFeedback::ExpireToasts(state_, elapsed_ms_);
  if (state_.toasts.size() != toast_count_before) {
    DirtyWindow();
  }

  if (compact_chat_dismiss_at_ms_ >= 0.f && elapsed_ms_ >= compact_chat_dismiss_at_ms_) {
    compact_chat_dismiss_at_ms_ = -1.f;
    CloseCompactChat();
  }

  const LayoutMode previous = state_.layout_mode;
  ApplyLayoutModeFromContext(context);
  if (previous != state_.layout_mode) {
    OnLayoutModeChanged();
    SaveFocus();
    SyncLayout();
    RestoreFocus();
    return;
  }
  DirtyWindow();
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
  Instance().CloseCompactChat();
}

void ShellHost::PopTransientCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                     const Rml::VariantList& /*args*/) {
  Instance().PopTransient();
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

} // namespace pbr
