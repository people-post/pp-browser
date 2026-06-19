#include "ui/ShellHost.h"

#include "platform/BrowserThread.h"
#include "ui/DataModelHost.h"
#include "ui/RmlMount.h"
#include "ui/ShellFeedback.h"
#include "ui/ShellInterruption.h"
#include "ui/ShellLayout.h"
#include "ui/ViewCatalog.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
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
    ctor.Bind("secondary_drawer_open", &host.state_.secondary_drawer_open);
    ctor.Bind("auxiliary_open", &host.state_.auxiliary_open);
    ctor.Bind("auxiliary_available", &host.state_.auxiliary_available);
    ctor.Bind("transient_active", &host.state_.transient_active);
    ctor.Bind("toolbar_title", &host.state_.toolbar_title);
    ctor.Bind("banner_message", &host.state_.banner_message);
    ctor.Bind("toasts", &host.state_.toasts);
    ctor.Bind("dialog_active", &host.state_.dialog.active);
    ctor.Bind("dialog_title", &host.state_.dialog.title);
    ctor.Bind("dialog_message", &host.state_.dialog.message);
    ctor.Bind("dialog_show_cancel", &host.state_.dialog.show_cancel);
    ctor.Bind("activity_visible", &host.state_.activity_visible);

    ctor.BindEventCallback("toggle_secondary", &ShellHost::ToggleSecondaryCallback);
    ctor.BindEventCallback("toggle_auxiliary", &ShellHost::ToggleAuxiliaryCallback);
    ctor.BindEventCallback("open_auxiliary", &ShellHost::OpenAuxiliaryCallback);
    ctor.BindEventCallback("transient_back", &ShellHost::PopTransientCallback);
    ctor.BindEventCallback("close_layer", &ShellHost::CloseLayerCallback);
    ctor.BindEventCallback("dismiss_banner", &ShellHost::DismissBannerCallback);
    ctor.BindEventCallback("dialog_ok", &ShellHost::DialogOkCallback);
    ctor.BindEventCallback("dialog_cancel", &ShellHost::DialogCancelCallback);
  });
}

void ShellHost::Initialize(Rml::Context* context) {
  context_ = context;
  state_ = {};
  state_.layout_mode = LayoutMode::Expanded;
  ShellLayout::SyncLayoutModeString(state_);
  last_synced_mode_ = LayoutMode::Expanded;
  next_pane_id_ = 1;
  next_overlay_id_ = 1;
  elapsed_ms_ = 0.f;
  saved_focus_id_.clear();
  sync_pending_ = false;
  restore_focus_after_sync_ = false;
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
  if (available && !was_available) {
    if (state_.layout_mode == LayoutMode::Expanded) {
      state_.auxiliary_open = true;
      RequestSyncLayout();
    } else {
      ShellFeedback::ShowToast(state_, "Preview ready — tap Preview to open.", ToastDuration::Short, elapsed_ms_);
      DirtyWindow();
    }
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

void ShellHost::ToggleSecondary() {
  if (state_.layout_mode == LayoutMode::Compact) {
    state_.secondary_drawer_open = !state_.secondary_drawer_open;
    RequestSyncLayout();
    return;
  }
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
  RequestSyncLayout();
  DirtyWindow();
  return true;
}

void ShellHost::DirtyWindow() {
  DataModelHost::Instance().Dirty("window", "layout_mode");
  DataModelHost::Instance().Dirty("window", "secondary_drawer_open");
  DataModelHost::Instance().Dirty("window", "auxiliary_open");
  DataModelHost::Instance().Dirty("window", "auxiliary_available");
  DataModelHost::Instance().Dirty("window", "transient_active");
  DataModelHost::Instance().Dirty("window", "toolbar_title");
  DataModelHost::Instance().Dirty("window", "banner_message");
  DataModelHost::Instance().Dirty("window", "toasts");
  DataModelHost::Instance().Dirty("window", "dialog_active");
  DataModelHost::Instance().Dirty("window", "dialog_title");
  DataModelHost::Instance().Dirty("window", "dialog_message");
  DataModelHost::Instance().Dirty("window", "dialog_show_cancel");
  DataModelHost::Instance().Dirty("window", "activity_visible");
}

void ShellHost::SetActivityVisible(bool visible) {
  state_.activity_visible = visible;
  DirtyWindow();
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
}

void ShellHost::OnLayoutModeChanged() {
  if (state_.layout_mode == LayoutMode::Expanded) {
    state_.secondary_drawer_open = false;
  }
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
  for (const PaneState& pane : state_.panes) {
    if (pane.spec.role == PaneRole::Secondary) {
      out << SerializePaneSlot(pane.spec.key, "shell-pane-secondary");
    }
  }
  for (const PaneState& pane : state_.panes) {
    if (pane.spec.role == PaneRole::Primary) {
      out << SerializePaneSlot(pane.spec.key, "shell-pane-primary", pane.spec.provides_composer);
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

  for (const PaneState& pane : state_.panes) {
    if (pane.spec.role == PaneRole::Primary) {
      out << SerializePaneSlot(pane.spec.key, "shell-pane-primary shell-pane-primary-compact");
    }
  }

  if (state_.secondary_drawer_open) {
    out << "<div class=\"shell-drawer-scrim\" data-event-click=\"toggle_secondary()\"></div>";
    out << "<div class=\"shell-drawer shell-drawer-secondary\">";
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Secondary) {
        out << SerializePaneSlot(pane.spec.key, nullptr);
      }
    }
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

  if (!state_.transient_active) {
    const bool has_composer = std::any_of(state_.panes.begin(), state_.panes.end(),
                                          [](const PaneState& pane) { return pane.spec.provides_composer; });
    out << "<div class=\"shell-bottom-chrome\">";
    if (has_composer) {
      out << "<div class=\"shell-composer-mount\" id=\"shell-composer-mount\"></div>";
    }
    out << "<div class=\"shell-toolbar\">";
    for (const PaneState& pane : state_.panes) {
      if (pane.spec.role == PaneRole::Secondary && !pane.spec.toolbar_label.empty()) {
        out << "<button class=\"shell-toolbar-btn\" data-event-click=\"toggle_secondary()\">"
            << pane.spec.toolbar_label.c_str() << "</button>";
        break;
      }
    }
    out << "<div class=\"shell-toolbar-flex\"></div>";
    out << "<span class=\"shell-toolbar-title\" data-rml=\"toolbar_title\"></span>";
    out << "<div class=\"shell-toolbar-flex\"></div>";
    out << "<button class=\"shell-toolbar-btn shell-toolbar-btn-preview\" data-if=\"auxiliary_available\" "
           "data-event-click=\"toggle_auxiliary()\">Preview</button>";
    out << "</div></div>";
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
         "<svg src=\"../icons/back.svg\" width=\"18\" height=\"18\" crop-to-content=\"true\"></svg>"
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

void ShellHost::MountComposer() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  const PaneState* composer_pane = nullptr;
  for (const PaneState& pane : state_.panes) {
    if (pane.spec.provides_composer) {
      composer_pane = &pane;
      break;
    }
  }
  if (!composer_pane) {
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

  if (state_.transient_active) {
    return;
  }
  Rml::Element* target = doc->GetElementById("shell-composer-mount");
  if (target) {
    RmlMount::MountInner(target, body);
  }
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

  for (const PaneState& pane : state_.panes) {
    mount_key(pane.spec.key);
  }
  if (!state_.transient_stack.empty()) {
    mount_key(state_.transient_stack.back().spec.key);
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
  MountComposer();
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
}

void ShellHost::Update(Rml::Context* context) {
  elapsed_ms_ += 16.f;
  const size_t toast_count_before = state_.toasts.size();
  ShellFeedback::ExpireToasts(state_, elapsed_ms_);
  if (state_.toasts.size() != toast_count_before) {
    DirtyWindow();
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

void ShellHost::ToggleSecondaryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                        const Rml::VariantList& /*args*/) {
  Instance().ToggleSecondary();
}

void ShellHost::ToggleAuxiliaryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().ToggleAuxiliary();
}

void ShellHost::OpenAuxiliaryCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                      const Rml::VariantList& /*args*/) {
  Instance().OpenAuxiliary();
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

} // namespace pbr
