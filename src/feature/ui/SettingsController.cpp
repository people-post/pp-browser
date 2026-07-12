#include "feature/ui/SettingsController.h"

#include "base/data/SessionStore.h"
#include "base/platform/BrowserThread.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/settings/ProfileSettingsSection.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>

namespace pbr {

namespace {

constexpr uint64_t kDebounceMs = 500;
constexpr uint64_t kToastSuppressMs = 2000;

Rml::String EventValue(Rml::Event& ev) {
  return ev.GetParameter<Rml::String>("value", Rml::String());
}

} // namespace

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
  section_handlers_ = CreateSettingsSections();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    section_handlers_by_id_[handler->Id()] = handler.get();
  }
  InitSections();
}

SettingsController& SettingsController::Instance() {
  static SettingsController controller;
  return controller;
}

SettingsSectionHandler* SettingsController::FindHandler(const std::string& section_id) {
  const auto it = section_handlers_by_id_.find(section_id);
  return it == section_handlers_by_id_.end() ? nullptr : it->second;
}

const SettingsSectionHandler* SettingsController::FindHandler(const std::string& section_id) const {
  const auto it = section_handlers_by_id_.find(section_id);
  return it == section_handlers_by_id_.end() ? nullptr : it->second;
}

void SettingsController::InitSections() {
  sections_.clear();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    if (std::string(handler->Id()) == "profile") {
      continue;
    }
    const SettingsSectionListItem item = handler->ListItem();
    sections_.push_back({.id = item.id.c_str(), .title = item.title.c_str(), .subtitle = item.subtitle.c_str()});
  }
}

void SettingsController::PullBindingsToUiState() {
  ui_state_.llm_preset = bindings_.llm_preset.c_str();
  ui_state_.llm_base_url = bindings_.llm_base_url.c_str();
  ui_state_.llm_model = bindings_.llm_model.c_str();
  ui_state_.llm_api_key = bindings_.llm_api_key.c_str();
  ui_state_.llm_api_key_env = bindings_.llm_api_key_env.c_str();
  ui_state_.promoted_mcp_url = bindings_.promoted_mcp_url.c_str();
  ui_state_.search_provider = bindings_.search_provider.c_str();
  ui_state_.relay_base_url = bindings_.relay_base_url.c_str();
  ui_state_.directory_base_url = bindings_.directory_base_url.c_str();
  ui_state_.registration_base_url = bindings_.registration_base_url.c_str();
  ui_state_.profile_nickname = bindings_.profile_nickname.c_str();
  ui_state_.profile_peer_id = bindings_.profile_peer_id.c_str();
  ui_state_.profile_relay_id = bindings_.profile_relay_id.c_str();
  ui_state_.profile_public_key = bindings_.profile_public_key.c_str();
  ui_state_.profile_registered = bindings_.profile_registered.c_str();
  ui_state_.appearance = bindings_.appearance.c_str();

  ui_state_.mcp_servers.clear();
  ui_state_.mcp_servers.reserve(bindings_.mcp_servers.size());
  for (const McpServerRow& row : bindings_.mcp_servers) {
    ui_state_.mcp_servers.push_back({.id = row.id.c_str(),
                                     .url = row.url.c_str(),
                                     .command = row.command.c_str(),
                                     .args_text = row.args_text.c_str(),
                                     .enabled = row.enabled});
  }
}

void SettingsController::PushUiStateToBindings() {
  bindings_.llm_preset = ui_state_.llm_preset.c_str();
  bindings_.llm_base_url = ui_state_.llm_base_url.c_str();
  bindings_.llm_model = ui_state_.llm_model.c_str();
  bindings_.llm_api_key = ui_state_.llm_api_key.c_str();
  bindings_.llm_api_key_env = ui_state_.llm_api_key_env.c_str();
  bindings_.promoted_mcp_url = ui_state_.promoted_mcp_url.c_str();
  bindings_.search_provider = ui_state_.search_provider.c_str();
  bindings_.relay_base_url = ui_state_.relay_base_url.c_str();
  bindings_.directory_base_url = ui_state_.directory_base_url.c_str();
  bindings_.registration_base_url = ui_state_.registration_base_url.c_str();
  bindings_.profile_nickname = ui_state_.profile_nickname.c_str();
  bindings_.profile_peer_id = ui_state_.profile_peer_id.c_str();
  bindings_.profile_relay_id = ui_state_.profile_relay_id.c_str();
  bindings_.profile_public_key = ui_state_.profile_public_key.c_str();
  bindings_.profile_registered = ui_state_.profile_registered.c_str();
  bindings_.appearance = ui_state_.appearance.c_str();
  bindings_.profile_label = ui_state_.profile_label.c_str();
  bindings_.config_dir = ui_state_.config_dir.c_str();
  bindings_.data_dir = ui_state_.data_dir.c_str();
  bindings_.profile_dir = ui_state_.profile_dir.c_str();
  bindings_.pin_protection_status = ui_state_.pin_protection_status.c_str();
  bindings_.security_can_change_pin = ui_state_.security_can_change_pin;

  bindings_.mcp_servers.clear();
  bindings_.mcp_servers.reserve(ui_state_.mcp_servers.size());
  for (const McpServerUiState& row : ui_state_.mcp_servers) {
    bindings_.mcp_servers.push_back({.id = row.id.c_str(),
                                      .url = row.url.c_str(),
                                      .command = row.command.c_str(),
                                      .args_text = row.args_text.c_str(),
                                      .enabled = row.enabled});
  }
}

void SettingsController::SyncBindingsFromSession() {
  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    handler->SyncFromSession(bootstrap, ui_state_);
  }
  PushUiStateToBindings();
}

void SettingsController::ReloadFromDisk() {
  if (auto reloaded = SessionStore::Instance().ReloadFromDisk(); !reloaded) {
    status_ = reloaded.error().message;
    log().warning << "ReloadFromDisk failed: " << status_.c_str();
    return;
  }
  status_ = "";
  SyncBindingsFromSession();
}

bool SettingsController::RegisterModel(Rml::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "settings", [](Rml::DataModelConstructor& ctor) {
    auto& controller = SettingsController::Instance();
    if (auto section_handle = ctor.RegisterStruct<SectionListRow>()) {
      section_handle.RegisterMember("id", &SectionListRow::id);
      section_handle.RegisterMember("title", &SectionListRow::title);
      section_handle.RegisterMember("subtitle", &SectionListRow::subtitle);
    }
    if (auto mcp_handle = ctor.RegisterStruct<McpServerRow>()) {
      mcp_handle.RegisterMember("id", &McpServerRow::id);
      mcp_handle.RegisterMember("url", &McpServerRow::url);
      mcp_handle.RegisterMember("command", &McpServerRow::command);
      mcp_handle.RegisterMember("args_text", &McpServerRow::args_text);
      mcp_handle.RegisterMember("enabled", &McpServerRow::enabled);
    }
    ctor.RegisterArray<std::vector<SectionListRow>>();
    ctor.RegisterArray<std::vector<McpServerRow>>();
    ctor.Bind("sections", &controller.sections_);
    ctor.Bind("selected_id", &controller.selected_id_);
    ctor.Bind("selected_title", &controller.selected_title_);
    ctor.Bind("compact_layout", &controller.compact_layout_);
    ctor.Bind("show_detail", &controller.show_detail_);
    ctor.Bind("llm_preset", &controller.bindings_.llm_preset);
    ctor.Bind("llm_base_url", &controller.bindings_.llm_base_url);
    ctor.Bind("llm_model", &controller.bindings_.llm_model);
    ctor.Bind("llm_api_key", &controller.bindings_.llm_api_key);
    ctor.Bind("llm_api_key_env", &controller.bindings_.llm_api_key_env);
    ctor.Bind("promoted_mcp_url", &controller.bindings_.promoted_mcp_url);
    ctor.Bind("search_provider", &controller.bindings_.search_provider);
    ctor.Bind("mcp_servers", &controller.bindings_.mcp_servers);
    ctor.Bind("relay_base_url", &controller.bindings_.relay_base_url);
    ctor.Bind("directory_base_url", &controller.bindings_.directory_base_url);
    ctor.Bind("registration_base_url", &controller.bindings_.registration_base_url);
    ctor.Bind("profile_nickname", &controller.bindings_.profile_nickname);
    ctor.Bind("profile_peer_id", &controller.bindings_.profile_peer_id);
    ctor.Bind("profile_relay_id", &controller.bindings_.profile_relay_id);
    ctor.Bind("profile_public_key", &controller.bindings_.profile_public_key);
    ctor.Bind("profile_registered", &controller.bindings_.profile_registered);
    ctor.Bind("appearance", &controller.bindings_.appearance);
    ctor.Bind("profile_label", &controller.bindings_.profile_label);
    ctor.Bind("config_dir", &controller.bindings_.config_dir);
    ctor.Bind("data_dir", &controller.bindings_.data_dir);
    ctor.Bind("profile_dir", &controller.bindings_.profile_dir);
    ctor.Bind("pin_protection_status", &controller.bindings_.pin_protection_status);
    ctor.Bind("security_can_change_pin", &controller.bindings_.security_can_change_pin);
    ctor.Bind("pin_change_old", &controller.bindings_.pin_change_old);
    ctor.Bind("pin_change_new", &controller.bindings_.pin_change_new);
    ctor.Bind("pin_change_confirm", &controller.bindings_.pin_change_confirm);
    ctor.Bind("status", &controller.status_);
    ctor.BindEventCallback("select_section", &SettingsController::SelectSectionCallback);
    ctor.BindEventCallback("back_to_list", &SettingsController::BackToListCallback);
    ctor.BindEventCallback("reset_section", &SettingsController::ResetSectionCallback);
    ctor.BindEventCallback("on_llm_field_changed", &SettingsController::OnLlmFieldChangedCallback);
    ctor.BindEventCallback("on_llm_preset_changed", &SettingsController::OnLlmPresetChangedCallback);
    ctor.BindEventCallback("on_appearance_changed", &SettingsController::OnAppearanceChangedCallback);
    ctor.BindEventCallback("on_integrations_field_changed", &SettingsController::OnIntegrationsFieldChangedCallback);
    ctor.BindEventCallback("on_network_field_changed", &SettingsController::OnNetworkFieldChangedCallback);
    ctor.BindEventCallback("on_profile_field_changed", &SettingsController::OnProfileFieldChangedCallback);
    ctor.BindEventCallback("register_profile", &SettingsController::OnRegisterProfileCallback);
    ctor.BindEventCallback("copy_profile_id", &SettingsController::OnCopyProfileIdCallback);
    ctor.BindEventCallback("share_profile", &SettingsController::OnShareProfileCallback);
    ctor.BindEventCallback("add_mcp_server", &SettingsController::OnAddMcpServerCallback);
    ctor.BindEventCallback("remove_mcp_server", &SettingsController::OnRemoveMcpServerCallback);
    ctor.BindEventCallback("change_pin", &SettingsController::OnChangePinCallback);
  });
}

void SettingsController::DirtyAll() {
  auto& host = DataModelHost::Instance();
  host.Dirty("settings", "sections");
  host.Dirty("settings", "selected_id");
  host.Dirty("settings", "selected_title");
  host.Dirty("settings", "compact_layout");
  host.Dirty("settings", "show_detail");
  host.Dirty("settings", "llm_preset");
  host.Dirty("settings", "llm_base_url");
  host.Dirty("settings", "llm_model");
  host.Dirty("settings", "llm_api_key");
  host.Dirty("settings", "llm_api_key_env");
  host.Dirty("settings", "promoted_mcp_url");
  host.Dirty("settings", "search_provider");
  host.Dirty("settings", "mcp_servers");
  host.Dirty("settings", "relay_base_url");
  host.Dirty("settings", "directory_base_url");
  host.Dirty("settings", "registration_base_url");
  host.Dirty("settings", "profile_nickname");
  host.Dirty("settings", "profile_peer_id");
  host.Dirty("settings", "profile_relay_id");
  host.Dirty("settings", "profile_public_key");
  host.Dirty("settings", "profile_registered");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "pin_protection_status");
  host.Dirty("settings", "security_can_change_pin");
  host.Dirty("settings", "pin_change_old");
  host.Dirty("settings", "pin_change_new");
  host.Dirty("settings", "pin_change_confirm");
  host.Dirty("settings", "status");
}

void SettingsController::FinishPaneResync() {
  SyncBindingsFromSession();
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  // Select widgets can emit a spurious change on the frame after remount.
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    if (ShellHost::Instance().State().nav_tab != NavTab::Me) {
      suppress_auto_save_ = false;
      return;
    }
    SyncBindingsFromSession();
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    suppress_auto_save_ = false;
  });
}

void SettingsController::OnShellLayoutSynced() {
  if (!suppress_auto_save_) {
    return;
  }
  if (ShellHost::Instance().State().nav_tab != NavTab::Me) {
    suppress_auto_save_ = false;
    return;
  }
  FinishPaneResync();
}

void SettingsController::OnNavTabActivated() {
  log().info << "OnNavTabActivated";
  dirty_sections_.clear();
  debounce_deadline_ms_ = 0;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  ReloadFromDisk();
  suppress_auto_save_ = true;
}

void SettingsController::OnNavTabDeactivated() {
  FlushPending();
}

void SettingsController::SyncLayoutMode() {
  const bool compact = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }

  log().info << "SyncLayoutMode compact=" << compact;
  suppress_auto_save_ = true;
  compact_layout_ = compact;
  if (!compact) {
    show_detail_ = false;
    if (!selected_id_.empty()) {
      ShellHost::Instance().SetPrimaryPane("settings_detail");
      BrowserThread::RunUITasks();
    }
  } else if (!selected_id_.empty()) {
    show_detail_ = true;
    ShellHost::Instance().ClearPrimaryPane();
    BrowserThread::RunUITasks();
  }
  DirtyAll();
  suppress_auto_save_ = false;
}

void SettingsController::OpenSettings() {
  const bool already_me = ShellHost::Instance().State().nav_tab == NavTab::Me;
  ShellHost::Instance().SelectNavTab(NavTab::Me);
  if (already_me) {
    OnNavTabActivated();
    FinishPaneResync();
  }
}

void SettingsController::MarkSectionDirty(const std::string& section_id) {
  if (suppress_auto_save_) {
    log().info << "MarkSectionDirty(" << section_id << ") suppressed during UI transition";
    return;
  }

  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  dirty_sections_.insert(section_id);
  if (handler->FlushMode() == SettingsFlushMode::Debounced) {
    debounce_deadline_ms_ = SDL_GetTicks() + kDebounceMs;
    return;
  }

  FlushSection(section_id);
}

void SettingsController::FlushPending() {
  FlushAllDirty();
}

void SettingsController::FlushAllDirty() {
  if (dirty_sections_.empty()) {
    debounce_deadline_ms_ = 0;
    return;
  }

  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    if (dirty_sections_.count(handler->Id()) == 0) {
      continue;
    }
    if (!FlushSection(handler->Id())) {
      return;
    }
  }
}

void SettingsController::Tick() {
  if (debounce_deadline_ms_ == 0 || dirty_sections_.empty()) {
    return;
  }
  if (SDL_GetTicks() >= debounce_deadline_ms_) {
    debounce_deadline_ms_ = 0;
    for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
      if (handler->FlushMode() != SettingsFlushMode::Debounced) {
        continue;
      }
      if (dirty_sections_.count(handler->Id()) == 0) {
        continue;
      }
      if (!FlushSection(handler->Id())) {
        return;
      }
    }
  }
}

bool SettingsController::FlushSection(const std::string& section_id) {
  if (suppress_auto_save_) {
    log().info << "FlushSection(" << section_id << ") suppressed during UI transition";
    return true;
  }

  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    dirty_sections_.erase(section_id);
    if (dirty_sections_.empty()) {
      debounce_deadline_ms_ = 0;
    }
    return true;
  }

  log().info << "FlushSection(" << section_id << ")";

  PullBindingsToUiState();
  if (auto flushed = handler->Flush(ui_state_, SessionStore::Instance()); !flushed) {
    status_ = flushed.error().message;
    DataModelHost::Instance().Dirty("settings", "status");
    return false;
  }

  dirty_sections_.erase(section_id);
  if (dirty_sections_.empty()) {
    debounce_deadline_ms_ = 0;
  }

  status_ = "";
  PushUiStateToBindings();
  DirtyAll();
  MaybeShowSaveToast(section_id);
  ShellHost::Instance().DirtyWindow();
  return true;
}

void SettingsController::MaybeShowSaveToast(const std::string& section_id) {
  const uint64_t now = SDL_GetTicks();
  if (last_toast_section_ == section_id && now - last_toast_at_ms_ < kToastSuppressMs) {
    return;
  }
  last_toast_section_ = section_id;
  last_toast_at_ms_ = now;
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Settings saved");
}

void SettingsController::OnResetSection(const std::string& section_id) {
  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  handler->ResetToDefaults(ui_state_, SessionStore::Instance());
  PushUiStateToBindings();
  DirtyAll();
  FlushSection(section_id);
}

void SettingsController::CompleteSectionSelection(bool expanded) {
  suppress_auto_save_ = true;
  if (expanded) {
    ShellHost::Instance().SetPrimaryPane("settings_detail");
    BrowserThread::RunUITasks();
  } else {
    FinishPaneResync();
  }
  log().info << "CompleteSectionSelection id=" << selected_id_.c_str()
             << " expanded=" << expanded;
}

void SettingsController::OnSelectSection(const std::string& section_id) {
  log().info << "OnSelectSection(" << section_id << ")";
  FlushPending();

  selected_id_.clear();
  selected_title_.clear();
  for (const SectionListRow& section : sections_) {
    if (section.id == section_id.c_str()) {
      selected_id_ = section.id;
      selected_title_ = section.title;
      break;
    }
  }

  status_ = "";
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_) {
    show_detail_ = true;
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { CompleteSectionSelection(false); });
  } else {
    show_detail_ = false;
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() { CompleteSectionSelection(true); });
  }
}

void SettingsController::OnBackToList() {
  log().info << "OnBackToList";
  FlushPending();
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  status_ = "";
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    suppress_auto_save_ = true;
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    suppress_auto_save_ = false;
  });
}

void SettingsController::SelectSectionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& args) {
  if (args.empty()) {
    SettingsController::Instance().log().warning << "select_section called with no args";
    return;
  }
  if (args[0].GetType() != Rml::Variant::STRING) {
    SettingsController::Instance().log().warning << "select_section arg type="
                                              << static_cast<int>(args[0].GetType());
    return;
  }
  Instance().OnSelectSection(std::string(args[0].Get<Rml::String>().c_str()));
}

void SettingsController::BackToListCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                            const Rml::VariantList& /*args*/) {
  Instance().OnBackToList();
}

void SettingsController::ResetSectionCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& args) {
  if (args.empty() || args[0].GetType() != Rml::Variant::STRING) {
    return;
  }
  Instance().OnResetSection(std::string(args[0].Get<Rml::String>().c_str()));
}

void SettingsController::OnLlmFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("llm");
}

void SettingsController::OnLlmPresetChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                    const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (controller.suppress_auto_save_) {
    return;
  }

  const Rml::String value = EventValue(ev);
  if (!value.empty()) {
    controller.bindings_.llm_preset = value;
  }

  controller.PullBindingsToUiState();
  const SettingsSectionHandler* handler = controller.FindHandler("llm");
  if (!handler) {
    return;
  }
  if (handler->IsPersisted(controller.ui_state_, SessionStore::Instance().Snapshot())) {
    return;
  }

  controller.FlushSection("llm");
}

void SettingsController::OnAppearanceChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                       const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (controller.suppress_auto_save_) {
    return;
  }

  const Rml::String value = EventValue(ev);
  if (!value.empty()) {
    controller.bindings_.appearance = value;
  }

  controller.PullBindingsToUiState();
  const SettingsSectionHandler* handler = controller.FindHandler("appearance");
  if (!handler) {
    return;
  }
  if (handler->IsPersisted(controller.ui_state_, SessionStore::Instance().Snapshot())) {
    return;
  }

  controller.MarkSectionDirty("appearance");
}

void SettingsController::OnIntegrationsFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                            const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("integrations");
}

void SettingsController::OnNetworkFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                       const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("network");
}

void SettingsController::OnProfileFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                       const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("profile");
}

void SettingsController::OnRegisterProfileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  Instance().OnRegisterProfile();
}

void SettingsController::OnCopyProfileIdCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                 const Rml::VariantList& /*args*/) {
  Instance().OnCopyProfileId();
}

void SettingsController::OnShareProfileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnShareProfile();
}

void SettingsController::OnAddMcpServerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                              const Rml::VariantList& /*args*/) {
  Instance().OnAddMcpServer();
}

void SettingsController::OnRemoveMcpServerCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& args) {
  if (args.empty()) {
    return;
  }
  Instance().OnRemoveMcpServer(static_cast<int>(args[0].Get<int>(0)));
}

void SettingsController::OnRegisterProfile() {
  PullBindingsToUiState();
  PinGateController::Instance().EnsureUnlocked([this](const bool unlocked) {
    if (!unlocked) {
      status_ = "PIN required to register";
      DataModelHost::Instance().Dirty("settings", "status");
      return;
    }
    if (auto registered = ProfileSettingsSection::RegisterIdentity(ui_state_); !registered) {
      status_ = registered.error().message;
      DataModelHost::Instance().Dirty("settings", "status");
      return;
    }
    status_ = "";
    PushUiStateToBindings();
    DirtyAll();
    MaybeShowSaveToast("profile");
  });
}

void SettingsController::OnCopyProfileId() {
  const std::string peer_id = bindings_.profile_peer_id.c_str();
  if (peer_id.empty()) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "No Peer ID yet");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(bindings_.profile_peer_id);
  }
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Peer ID copied");
  ShellHost::Instance().DirtyWindow();
}

void SettingsController::OnShareProfile() {
  const std::string peer_id = bindings_.profile_peer_id.c_str();
  if (peer_id.empty()) {
    ShellFeedback::ShowToast(ShellHost::Instance().State(), "No Peer ID yet");
    ShellHost::Instance().DirtyWindow();
    return;
  }
  const std::string nickname = bindings_.profile_nickname.c_str();
  const std::string relay_id = bindings_.profile_relay_id.c_str();
  std::string invite = nickname.empty() ? peer_id : (nickname + " (" + peer_id + ")");
  if (!relay_id.empty()) {
    invite += " [" + relay_id + "]";
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(invite.c_str());
  }
  ShellFeedback::ShowToast(ShellHost::Instance().State(), "Invite copied");
  ShellHost::Instance().DirtyWindow();
}

void SettingsController::OnAddMcpServer() {
  bindings_.mcp_servers.push_back({});
  ui_state_.mcp_servers.push_back({});
  DataModelHost::Instance().Dirty("settings", "mcp_servers");
  MarkSectionDirty("integrations");
}

void SettingsController::OnRemoveMcpServer(const int index) {
  if (index < 0 || index >= static_cast<int>(bindings_.mcp_servers.size())) {
    return;
  }
  bindings_.mcp_servers.erase(bindings_.mcp_servers.begin() + index);
  PullBindingsToUiState();
  if (index < static_cast<int>(ui_state_.mcp_servers.size())) {
    ui_state_.mcp_servers.erase(ui_state_.mcp_servers.begin() + index);
  }
  PushUiStateToBindings();
  DataModelHost::Instance().Dirty("settings", "mcp_servers");
  MarkSectionDirty("integrations");
}

void SettingsController::OnChangePinCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  Instance().OnChangePin();
}

void SettingsController::OnChangePin() {
  if (!MessagingHub::Instance().IsInitialized() || !MessagingHub::Instance().HasVault()) {
    status_ = "Set up key protection first";
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }
  if (!MessagingHub::Instance().AreSecretsReady()) {
    PinGateController::Instance().EnsureUnlocked([this](const bool unlocked) {
      if (!unlocked) {
        status_ = "Unlock profile PIN to change it";
        DataModelHost::Instance().Dirty("settings", "status");
        return;
      }
      OnChangePin();
    });
    return;
  }

  const std::string old_pin = bindings_.pin_change_old.c_str();
  const std::string new_pin = bindings_.pin_change_new.c_str();
  const std::string confirm = bindings_.pin_change_confirm.c_str();
  if (old_pin.empty() || new_pin.empty()) {
    status_ = "Current and new PIN are required";
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }
  if (new_pin != confirm) {
    status_ = "New PINs do not match";
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }
  if (new_pin.size() < 4) {
    status_ = "Use at least 4 characters";
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  DataKeyVault* vault = MessagingHub::Instance().Vault();
  if (vault == nullptr) {
    status_ = "Vault unavailable";
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }
  if (auto changed = vault->ChangePin(old_pin, new_pin); !changed) {
    status_ = changed.error().message.c_str();
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  ProfilePreferences prefs = SessionStore::Instance().Snapshot().profile_prefs;
  prefs.pin_is_default = false;
  if (auto saved = SessionStore::Instance().SaveProfilePrefs(prefs); !saved) {
    status_ = saved.error().message.c_str();
    DataModelHost::Instance().Dirty("settings", "status");
    return;
  }

  bindings_.pin_change_old = "";
  bindings_.pin_change_new = "";
  bindings_.pin_change_confirm = "";
  status_ = "PIN updated";
  SyncBindingsFromSession();
  DirtyAll();
}

} // namespace pbr
