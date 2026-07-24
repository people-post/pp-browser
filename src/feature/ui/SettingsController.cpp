#include "feature/ui/SettingsController.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/data/AppPaths.h"
#include "base/data/LlmPreset.h"
#include "base/data/SchemaVersion.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "base/platform/BrowserThread.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/ui/ChatSessionActions.h"
#include "feature/messaging/MessagingHub.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/ui/ContactsController.h"
#include "feature/ui/DataModelHost.h"
#include "feature/ui/PinGateController.h"
#include "feature/ui/ProfileSettingsSection.h"
#include "feature/ui/SecuritySettingsSection.h"
#include "feature/ui/ShellFeedback.h"
#include "feature/ui/ShellHost.h"
#include "feature/ui/UserFeedback.h"
#include "base/error/AppError.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>

#include <filesystem>

namespace pbr {

namespace {

constexpr uint64_t kDebounceMs = 500;
constexpr uint64_t kToastSuppressMs = 2000;

Rml::String EventValue(Rml::Event& ev) {
  return ev.GetParameter<Rml::String>("value", Rml::String());
}

/** Anchor ShowActions float menus under the right side of a settings choice row. */
Rml::Vector2i ChoiceRowMenuPosition(Rml::Event& ev) {
  Rml::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  Rml::Vector2i position{0, 0};
  if (!target) {
    return position;
  }

  Rml::Element* anchor = target;
  const int child_count = target->GetNumChildren();
  for (int i = 0; i < child_count; ++i) {
    Rml::Element* child = target->GetChild(i);
    if (child && child->IsClassSet("settings-choice-value")) {
      anchor = child;
      break;
    }
  }

  const Rml::Vector2f offset = anchor->GetAbsoluteOffset(Rml::BoxArea::Border);
  const Rml::Vector2f size = anchor->GetBox().GetSize(Rml::BoxArea::Border);
  // Match .context-menu-panel min-width so the menu's right edge lines up with the value.
  constexpr float kMenuMinWidthPx = 180.0f;
  position.x = static_cast<int>(offset.x + size.x - kMenuMinWidthPx);
  if (position.x < 0) {
    position.x = static_cast<int>(offset.x);
  }
  position.y = static_cast<int>(offset.y + size.y + 4.0f);
  return position;
}

} // namespace

SettingsController::SettingsController() {
  redirectLogger("SettingsController");
  section_handlers_.push_back(std::make_unique<ProfileSettingsSection>());
  for (auto& section : CreateSettingsSections()) {
    section_handlers_.push_back(std::move(section));
  }
  section_handlers_.push_back(std::make_unique<SecuritySettingsSection>());
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
  ui_state_.profile_registration_status = bindings_.profile_registration_status.c_str();
  ui_state_.profile_registration_expires = bindings_.profile_registration_expires.c_str();
  ui_state_.profile_register_label = bindings_.profile_register_label.c_str();
  ui_state_.profile_show_register = bindings_.profile_show_register;
  ui_state_.profile_show_rotate = bindings_.profile_show_rotate;
  ui_state_.auto_renew_registration = bindings_.auto_renew_registration.c_str();
  ui_state_.show_notifications = bindings_.show_notifications.c_str();
  ui_state_.brief_llm_key_masked = bindings_.brief_llm_key_masked.c_str();
  ui_state_.appearance = bindings_.appearance.c_str();
  ui_state_.appearance_label = bindings_.appearance_label.c_str();
  ui_state_.language = bindings_.language.c_str();
  ui_state_.language_label = bindings_.language_label.c_str();
  ui_state_.reduce_transparency = bindings_.reduce_transparency.c_str();
  ui_state_.pin_protection_status = bindings_.pin_protection_status.c_str();
  ui_state_.security_can_change_pin = bindings_.security_can_change_pin;
  ui_state_.group_invite_policy = bindings_.group_invite_policy.c_str();
  ui_state_.group_invite_policy_label = bindings_.group_invite_policy_label.c_str();

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
  bindings_.profile_registration_status = ui_state_.profile_registration_status.c_str();
  bindings_.profile_registration_expires = ui_state_.profile_registration_expires.c_str();
  bindings_.profile_register_label = ui_state_.profile_register_label.c_str();
  bindings_.profile_show_register = ui_state_.profile_show_register;
  bindings_.profile_show_rotate = ui_state_.profile_show_rotate;
  bindings_.auto_renew_registration = ui_state_.auto_renew_registration.c_str();
  bindings_.show_notifications = ui_state_.show_notifications.c_str();
  bindings_.brief_llm_key_masked = ui_state_.brief_llm_key_masked.c_str();
  bindings_.appearance = ui_state_.appearance.c_str();
  bindings_.appearance_label = ui_state_.appearance_label.c_str();
  bindings_.language = ui_state_.language.c_str();
  bindings_.language_label = ui_state_.language_label.c_str();
  bindings_.reduce_transparency = ui_state_.reduce_transparency.c_str();
  bindings_.profile_label = ui_state_.profile_label.c_str();
  bindings_.config_dir = ui_state_.config_dir.c_str();
  bindings_.data_dir = ui_state_.data_dir.c_str();
  bindings_.profile_dir = ui_state_.profile_dir.c_str();
  bindings_.profile_size_label = ui_state_.profile_size_label.c_str();
  bindings_.pin_protection_status = ui_state_.pin_protection_status.c_str();
  bindings_.security_can_change_pin = ui_state_.security_can_change_pin;
  bindings_.group_invite_policy = ui_state_.group_invite_policy.c_str();
  bindings_.group_invite_policy_label = ui_state_.group_invite_policy_label.c_str();
  bindings_.app_name = ui_state_.app_name.c_str();
  bindings_.app_version = ui_state_.app_version.c_str();

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
    log().warning << "ReloadFromDisk failed: " << AppError::Log(reloaded.error());
    ReportFailure(reloaded.error());
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
    ctor.Bind("in_account_sheet", &controller.in_account_sheet_);
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
    ctor.Bind("profile_registration_status", &controller.bindings_.profile_registration_status);
    ctor.Bind("profile_registration_expires", &controller.bindings_.profile_registration_expires);
    ctor.Bind("profile_register_label", &controller.bindings_.profile_register_label);
    ctor.Bind("profile_show_register", &controller.bindings_.profile_show_register);
    ctor.Bind("profile_show_rotate", &controller.bindings_.profile_show_rotate);
    ctor.Bind("auto_renew_registration", &controller.bindings_.auto_renew_registration);
    ctor.Bind("show_notifications", &controller.bindings_.show_notifications);
    ctor.Bind("brief_llm_key_masked", &controller.bindings_.brief_llm_key_masked);
    ctor.Bind("appearance", &controller.bindings_.appearance);
    ctor.Bind("appearance_label", &controller.bindings_.appearance_label);
    ctor.Bind("language", &controller.bindings_.language);
    ctor.Bind("language_label", &controller.bindings_.language_label);
    ctor.Bind("reduce_transparency", &controller.bindings_.reduce_transparency);
    ctor.Bind("profile_label", &controller.bindings_.profile_label);
    ctor.Bind("config_dir", &controller.bindings_.config_dir);
    ctor.Bind("data_dir", &controller.bindings_.data_dir);
    ctor.Bind("profile_dir", &controller.bindings_.profile_dir);
    ctor.Bind("profile_size_label", &controller.bindings_.profile_size_label);
    ctor.Bind("pin_protection_status", &controller.bindings_.pin_protection_status);
    ctor.Bind("security_can_change_pin", &controller.bindings_.security_can_change_pin);
    ctor.Bind("group_invite_policy", &controller.bindings_.group_invite_policy);
    ctor.Bind("group_invite_policy_label", &controller.bindings_.group_invite_policy_label);
    ctor.Bind("app_name", &controller.bindings_.app_name);
    ctor.Bind("app_version", &controller.bindings_.app_version);
    ctor.Bind("pin_change_old", &controller.bindings_.pin_change_old);
    ctor.Bind("pin_change_new", &controller.bindings_.pin_change_new);
    ctor.Bind("pin_change_confirm", &controller.bindings_.pin_change_confirm);
    ctor.Bind("status", &controller.status_);
    ctor.BindEventCallback("select_section", &SettingsController::SelectSectionCallback);
    ctor.BindEventCallback("back_to_list", &SettingsController::BackToListCallback);
    ctor.BindEventCallback("reset_section", &SettingsController::ResetSectionCallback);
    ctor.BindEventCallback("on_llm_field_changed", &SettingsController::OnLlmFieldChangedCallback);
    ctor.BindEventCallback("on_llm_preset_changed", &SettingsController::OnLlmPresetChangedCallback);
    ctor.BindEventCallback("on_choose_theme", &SettingsController::OnChooseThemeCallback);
    ctor.BindEventCallback("on_choose_language", &SettingsController::OnChooseLanguageCallback);
    ctor.BindEventCallback("on_choose_group_invite_policy", &SettingsController::OnChooseGroupInvitePolicyCallback);
    ctor.BindEventCallback("toggle_show_notifications", &SettingsController::ToggleShowNotificationsCallback);
    ctor.BindEventCallback("toggle_reduce_transparency", &SettingsController::ToggleReduceTransparencyCallback);
    ctor.BindEventCallback("toggle_auto_renew_registration", &SettingsController::ToggleAutoRenewRegistrationCallback);
    ctor.BindEventCallback("on_integrations_field_changed", &SettingsController::OnIntegrationsFieldChangedCallback);
    ctor.BindEventCallback("on_network_field_changed", &SettingsController::OnNetworkFieldChangedCallback);
    ctor.BindEventCallback("on_profile_nickname_commit", &SettingsController::OnProfileNicknameCommitCallback);
    ctor.BindEventCallback("register_profile", &SettingsController::OnRegisterProfileCallback);
    ctor.BindEventCallback("rotate_brief_llm_key", &SettingsController::OnRotateBriefLlmKeyCallback);
    ctor.BindEventCallback("copy_profile_id", &SettingsController::OnCopyProfileIdCallback);
    ctor.BindEventCallback("share_profile", &SettingsController::OnShareProfileCallback);
    ctor.BindEventCallback("add_mcp_server", &SettingsController::OnAddMcpServerCallback);
    ctor.BindEventCallback("remove_mcp_server", &SettingsController::OnRemoveMcpServerCallback);
    ctor.BindEventCallback("change_pin", &SettingsController::OnChangePinCallback);
    ctor.BindEventCallback("reset_profile", &SettingsController::OnResetProfileCallback);
  });
}

void SettingsController::DirtyAll(bool include_profile_nickname) {
  auto& host = DataModelHost::Instance();
  host.Dirty("settings", "sections");
  host.Dirty("settings", "selected_id");
  host.Dirty("settings", "selected_title");
  host.Dirty("settings", "in_account_sheet");
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
  if (include_profile_nickname) {
    host.Dirty("settings", "profile_nickname");
  }
  host.Dirty("settings", "profile_peer_id");
  host.Dirty("settings", "profile_relay_id");
  host.Dirty("settings", "profile_public_key");
  host.Dirty("settings", "profile_registered");
  host.Dirty("settings", "profile_registration_status");
  host.Dirty("settings", "profile_registration_expires");
  host.Dirty("settings", "profile_register_label");
  host.Dirty("settings", "profile_show_register");
  host.Dirty("settings", "profile_show_rotate");
  host.Dirty("settings", "auto_renew_registration");
  host.Dirty("settings", "show_notifications");
  host.Dirty("settings", "brief_llm_key_masked");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "appearance_label");
  host.Dirty("settings", "language");
  host.Dirty("settings", "language_label");
  host.Dirty("settings", "reduce_transparency");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "profile_size_label");
  host.Dirty("settings", "pin_protection_status");
  host.Dirty("settings", "security_can_change_pin");
  host.Dirty("settings", "group_invite_policy");
  host.Dirty("settings", "group_invite_policy_label");
  host.Dirty("settings", "app_name");
  host.Dirty("settings", "app_version");
  host.Dirty("settings", "pin_change_old");
  host.Dirty("settings", "pin_change_new");
  host.Dirty("settings", "pin_change_confirm");
  host.Dirty("settings", "status");
}

void SettingsController::FinishPaneResync() {
  // Keep any in-progress nickname across Sync+Dirty. On fresh start the session
  // nickname is still empty; a deferred resync after Me-tab remount can
  // otherwise push "" back into data-value and wipe characters mid-typing
  // (looks like HarfBuzz/lang glyph jitter). Chat has no equivalent path.
  auto resync_preserving_nickname = [this]() {
    const Rml::String live_nickname = bindings_.profile_nickname;
    SyncBindingsFromSession();
    bindings_.profile_nickname = live_nickname;
    DirtyAll(/*include_profile_nickname=*/false);
    if (context_) {
      context_->Update();
    }
  };

  resync_preserving_nickname();
  // Select widgets can emit a spurious change on the frame after remount.
  BrowserThread::PostTask(BrowserThreadId::UI, [this, resync_preserving_nickname]() {
    const ShellState& state = ShellHost::Instance().State();
    if (state.nav_tab != NavTab::Me && !state.account_sheet_open) {
      suppress_auto_save_ = false;
      return;
    }
    resync_preserving_nickname();
    suppress_auto_save_ = false;
  });
}

void SettingsController::OnShellLayoutSynced() {
  if (!suppress_auto_save_) {
    return;
  }
  const ShellState& state = ShellHost::Instance().State();
  if (state.nav_tab == NavTab::Me || state.account_sheet_open) {
    FinishPaneResync();
  } else {
    suppress_auto_save_ = false;
  }
}

void SettingsController::OnNavTabActivated() {
  log().info << "OnNavTabActivated";
  CommitProfileNickname(/*show_toast=*/false);
  FlushPending();
  dirty_sections_.clear();
  debounce_deadline_ms_ = 0;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  in_account_sheet_ = false;
  ShellHost::Instance().ClearLocalBack("settings_detail");
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  ReloadFromDisk();
  suppress_auto_save_ = true;
  DirtyAll();
  if (context_) {
    context_->Update();
  }
}

void SettingsController::OnAccountSheetOpened() {
  log().info << "OnAccountSheetOpened";
  dirty_sections_.clear();
  debounce_deadline_ms_ = 0;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  ShellHost::Instance().ClearLocalBack("settings_detail");
  in_account_sheet_ = true;
  ReloadFromDisk();
  suppress_auto_save_ = true;
}

void SettingsController::OnAccountSheetClosed() {
  CommitProfileNickname(/*show_toast=*/false);
  FlushPending();
  in_account_sheet_ = false;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  ShellHost::Instance().ClearLocalBack("settings_detail");
}

void SettingsController::SyncLayoutMode() {
  const bool compact = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }
  compact_layout_ = compact;

  ShellState& state = ShellHost::Instance().State();
  const Rml::String saved_id = selected_id_;
  const Rml::String saved_title = selected_title_;
  const bool had_detail = !saved_id.empty() || show_detail_;

  if (compact) {
    if (state.nav_tab == NavTab::Me) {
      ShellHost::Instance().ClearPrimaryPane();
      ShellHost::Instance().SelectNavTab(NavTab::Home);
      selected_id_ = saved_id;
      selected_title_ = saved_title;
      show_detail_ = had_detail;
      ShellHost::Instance().OpenAccountSheet();
      // OpenAccountSheet resets selection; restore sheet detail state.
      selected_id_ = saved_id;
      selected_title_ = saved_title;
      show_detail_ = had_detail;
      in_account_sheet_ = true;
      if (had_detail) {
        ShellHost::Instance().ClearLocalBack("settings_detail");
        ShellHost::Instance().PushLocalBack("settings_detail", [] {
          SettingsController::Instance().ApplyBackToListUi();
        });
      }
      DirtyAll();
      return;
    }
  } else if (state.account_sheet_open) {
    ShellHost::Instance().CloseAccountSheet();
    selected_id_ = saved_id;
    selected_title_ = saved_title;
    show_detail_ = false;
    in_account_sheet_ = false;
    ShellHost::Instance().SelectNavTab(NavTab::Me);
    // SelectNavTab activates Me and clears selection; restore for primary pane.
    selected_id_ = saved_id;
    selected_title_ = saved_title;
    if (had_detail && !selected_id_.empty()) {
      OpenSettingsDetailPane();
    }
    DirtyAll();
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

bool SettingsController::FlushSection(const std::string& section_id, bool show_toast) {
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
    log().warning << "FlushSection(" << section_id << ") failed: " << AppError::Log(flushed.error());
    ReportFailure(flushed.error());
    return false;
  }

  dirty_sections_.erase(section_id);
  if (dirty_sections_.empty()) {
    debounce_deadline_ms_ = 0;
  }

  status_ = "";
  // Keep the live nickname through Push/Dirty. SyncFromSession after flush can race
  // with continued typing when DirtyAll re-pushes profile_nickname into data-value
  // inputs (SetValue resets cursor/IME and looks like characters mutating). Same
  // preserve applies in FinishPaneResync (fresh-start empty identity wipe).
  const Rml::String live_nickname = bindings_.profile_nickname;
  const bool preserve_nickname = (section_id == "profile");
  PushUiStateToBindings();
  if (preserve_nickname) {
    bindings_.profile_nickname = live_nickname;
  }
  if (preserve_nickname && !show_toast) {
    // Silent nickname blur/close commit: skip DirtyAll/toast/DirtyWindow so the OSK
    // session is not restarted by chrome refresh.
    return true;
  }
  DirtyAll(!preserve_nickname);
  if (show_toast) {
    MaybeShowSaveToast(section_id);
  }
  ShellHost::Instance().DirtyWindow();
  return true;
}

void SettingsController::CommitProfileNickname(bool show_toast) {
  if (suppress_auto_save_) {
    return;
  }
  FlushSection("profile", show_toast);
}

void SettingsController::MaybeShowSaveToast(const std::string& section_id) {
  const uint64_t now = SDL_GetTicks();
  if (last_toast_section_ == section_id && now - last_toast_at_ms_ < kToastSuppressMs) {
    return;
  }
  last_toast_section_ = section_id;
  last_toast_at_ms_ = now;
  UserFeedback::Ok(Tr("settings.saved"));
}

void SettingsController::ReportFailure(const Error& err) {
  log().warning << "Settings failure: " << AppError::Log(err);
  status_ = UserFeedback::FailFrom(err).c_str();
  DataModelHost::Instance().Dirty("settings", "status");
}

void SettingsController::ReportFailure(const std::string& technical_message) {
  ReportFailure(Error(technical_message));
}

void SettingsController::OnResetSection(const std::string& section_id) {
  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  ShellFeedback::ShowConfirm(
      ShellHost::Instance().State(), Tr("settings.reset_defaults_confirm_title"),
      Tr("settings.reset_defaults_confirm_message"),
      [this, section_id](const bool ok) {
        if (!ok) {
          return;
        }
        PerformResetSection(section_id);
      });
  ShellHost::Instance().RequestSyncLayout();
}

void SettingsController::PerformResetSection(const std::string& section_id) {
  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  handler->ResetToDefaults(ui_state_, SessionStore::Instance());
  PushUiStateToBindings();
  DirtyAll();
  FlushSection(section_id);
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
  if (selected_id_.empty()) {
    return;
  }

  status_ = "";
  const bool sheet = ShellHost::Instance().State().account_sheet_open || in_account_sheet_;
  if (sheet) {
    show_detail_ = true;
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    ShellHost::Instance().PushLocalBack("settings_detail", [] {
      SettingsController::Instance().ApplyBackToListUi();
    });
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      suppress_auto_save_ = true;
      FinishPaneResync();
      ShellHost::Instance().RefreshDismissGestures();
      log().info << "OnSelectSection (sheet) complete id=" << selected_id_.c_str();
    });
    return;
  }

  show_detail_ = false;
  OpenSettingsDetailPane();
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    suppress_auto_save_ = true;
    FinishPaneResync();
    log().info << "OnSelectSection (pane) complete id=" << selected_id_.c_str();
  });
}

void SettingsController::OpenSettingsDetailPane() {
  compact_layout_ = ShellHost::Instance().State().layout_mode == LayoutMode::Compact;
  ShellState& state = ShellHost::Instance().State();
  auto is_settings_detail_transient = [](const ShellState& s) {
    return !s.transient_stack.empty() && s.transient_stack.back().spec.key == "settings_detail";
  };
  // Compact Me uses the account sheet, not transient panes.
  if (compact_layout_ || state.account_sheet_open) {
    return;
  }

  if (is_settings_detail_transient(state)) {
    ShellHost::Instance().PopTransient();
  }
  ShellHost::Instance().SetPrimaryPane("settings_detail");
}

bool SettingsController::CloseSettingsDetailPane() {
  ShellState& state = ShellHost::Instance().State();
  if (!state.transient_stack.empty() && state.transient_stack.back().spec.key == "settings_detail") {
    ShellHost::Instance().PopTransient();
    return true;
  }
  if (state.layout_mode != LayoutMode::Compact && state.nav_tab == NavTab::Me) {
    ShellHost::Instance().ClearPrimaryPane();
    return true;
  }
  return false;
}

void SettingsController::OnDetailDismissed() {
  FlushPending();
  selected_id_.clear();
  selected_title_.clear();
  show_detail_ = false;
  status_ = "";
  DirtyAll();
}

void SettingsController::ApplyBackToListUi() {
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
    ShellHost::Instance().RefreshDismissGestures();
  });
}

void SettingsController::OnBackToList() {
  log().info << "OnBackToList";
  FlushPending();
  if (ShellHost::Instance().HasLocalBack("settings_detail")) {
    ShellHost::Instance().RequestDismiss(DismissStyle::Instant);
    return;
  }
  if (in_account_sheet_ || ShellHost::Instance().State().account_sheet_open) {
    ApplyBackToListUi();
    return;
  }
  if (!CloseSettingsDetailPane()) {
    OnDetailDismissed();
  } else {
    OnDetailDismissed();
  }
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
  // Translate preset intent → precise default model before flush/normalize.
  const std::string preset = controller.ui_state_.llm_preset;
  if (const std::string default_model = DefaultModelForPreset(preset); !default_model.empty()) {
    controller.ui_state_.llm_model = default_model;
    controller.bindings_.llm_model = default_model.c_str();
  }
  if (preset != "custom") {
    // Known presets own base_url; clear custom URL so Normalize/ApplyPreset fill it.
    AppConfig scratch;
    ApplyPreset(scratch, preset, {});
    controller.ui_state_.llm_base_url = scratch.llm.base_url;
    controller.bindings_.llm_base_url = scratch.llm.base_url.c_str();
  }
  DataModelHost::Instance().Dirty("settings", "llm_model");
  DataModelHost::Instance().Dirty("settings", "llm_base_url");
  DataModelHost::Instance().Dirty("settings", "llm_preset");

  const SettingsSectionHandler* handler = controller.FindHandler("llm");
  if (!handler) {
    return;
  }
  if (handler->IsPersisted(controller.ui_state_, SessionStore::Instance().Snapshot())) {
    return;
  }

  controller.FlushSection("llm");
}

void SettingsController::OnChooseThemeCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                 const Rml::VariantList& /*args*/) {
  Instance().OnChooseTheme(ev);
}

void SettingsController::OnChooseTheme(Rml::Event& ev) {
  const Rml::Vector2i position = ChoiceRowMenuPosition(ev);

  const std::string current =
      bindings_.appearance.empty() ? "system" : std::string(bindings_.appearance.c_str());

  static const char* kThemeIds[] = {"system", "light", "dark"};
  std::vector<ContextMenuAction> actions;
  actions.reserve(3);
  for (const char* id : kThemeIds) {
    const std::string theme_id = id;
    actions.push_back({.id = theme_id,
                       .label = ThemeDisplayLabel(theme_id),
                       .enabled = {},
                       .run =
                           [this, theme_id]() {
                             ApplyThemeChoice(theme_id);
                           },
                       .icon = {},
                       .danger = false,
                       .selected = current == theme_id});
  }

  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyThemeChoice(const std::string& appearance_pref) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.appearance = appearance_pref.c_str();
  bindings_.appearance_label = ThemeDisplayLabel(appearance_pref).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("appearance");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnChooseLanguageCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                    const Rml::VariantList& /*args*/) {
  Instance().OnChooseLanguage(ev);
}

void SettingsController::OnChooseLanguage(Rml::Event& ev) {
  const Rml::Vector2i position = ChoiceRowMenuPosition(ev);

  auto& loc = LocalizationService::Instance();
  const std::string current = bindings_.language.empty() ? "system" : std::string(bindings_.language.c_str());

  std::vector<ContextMenuAction> actions;
  actions.push_back({.id = "system",
                     .label = loc.LanguageDisplayLabel("system"),
                     .enabled = {},
                     .run =
                         [this]() {
                           ApplyLanguageChoice("system");
                         },
                     .icon = {},
                     .danger = false,
                     .selected = current == "system"});

  for (const LocaleInfo& info : loc.AvailableLocales()) {
    const std::string tag = info.tag;
    actions.push_back({.id = tag,
                       .label = loc.LanguageDisplayLabel(tag),
                       .enabled = {},
                       .run =
                           [this, tag]() {
                             ApplyLanguageChoice(tag);
                           },
                       .icon = {},
                       .danger = false,
                       .selected = current == tag});
  }

  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyLanguageChoice(const std::string& language_pref) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.language = language_pref.c_str();
  bindings_.language_label = LocalizationService::Instance().LanguageDisplayLabel(language_pref).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("appearance");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnChooseGroupInvitePolicyCallback(Rml::DataModelHandle /*model*/, Rml::Event& ev,
                                                           const Rml::VariantList& /*args*/) {
  Instance().OnChooseGroupInvitePolicy(ev);
}

void SettingsController::OnChooseGroupInvitePolicy(Rml::Event& ev) {
  const Rml::Vector2i position = ChoiceRowMenuPosition(ev);

  const std::string current = bindings_.group_invite_policy.empty()
                                  ? "contacts_only"
                                  : std::string(bindings_.group_invite_policy.c_str());

  static const char* kPolicyIds[] = {"everyone", "contacts_only", "nobody"};
  std::vector<ContextMenuAction> actions;
  actions.reserve(3);
  for (const char* id : kPolicyIds) {
    const std::string policy_id = id;
    actions.push_back({.id = policy_id,
                       .label = GroupInvitePolicyDisplayLabel(policy_id),
                       .enabled = {},
                       .run =
                           [this, policy_id]() {
                             ApplyGroupInvitePolicyChoice(policy_id);
                           },
                       .icon = {},
                       .danger = false,
                       .selected = current == policy_id});
  }

  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyGroupInvitePolicyChoice(const std::string& policy) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.group_invite_policy = policy.c_str();
  bindings_.group_invite_policy_label = GroupInvitePolicyDisplayLabel(policy).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("security");
  FlushPending();
  DirtyAll();
}

void SettingsController::RefreshLocalizedChrome() {
  InitSections();
  SyncBindingsFromSession();
  if (!selected_id_.empty()) {
    if (const SettingsSectionHandler* handler = FindHandler(selected_id_.c_str())) {
      selected_title_ = handler->ListItem().title.c_str();
    }
  }
  DirtyAll();
}

void SettingsController::OnIntegrationsFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                            const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("integrations");
}

void SettingsController::OnNetworkFieldChangedCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                       const Rml::VariantList& /*args*/) {
  Instance().MarkSectionDirty("network");
}

void SettingsController::OnProfileNicknameCommitCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                         const Rml::VariantList& /*args*/) {
  // Persist on blur so soft-keyboard sessions are not interrupted by debounced
  // flush → toast/DirtyAll (dismisses and re-shows the OSK each keystroke).
  Instance().CommitProfileNickname(/*show_toast=*/false);
}

void SettingsController::ToggleShowNotificationsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                         const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.bindings_.show_notifications =
      controller.bindings_.show_notifications == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("profile");
  controller.DirtyAll();
}

void SettingsController::ToggleReduceTransparencyCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                          const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.bindings_.reduce_transparency =
      controller.bindings_.reduce_transparency == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("appearance");
  controller.DirtyAll();
}

void SettingsController::ToggleAutoRenewRegistrationCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                             const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.bindings_.auto_renew_registration =
      controller.bindings_.auto_renew_registration == "auto" ? "off" : "auto";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("profile");
  controller.DirtyAll();
}

void SettingsController::OnRegisterProfileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  Instance().OnRegisterProfile();
}

void SettingsController::OnRotateBriefLlmKeyCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                     const Rml::VariantList& /*args*/) {
  Instance().OnRotateBriefLlmKey();
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
  const bool renewing = ui_state_.profile_registered == "yes";
  PinGateController::Instance().EnsureUnlocked([this, renewing](const bool unlocked) {
    if (!unlocked) {
      ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to register"));
      return;
    }
    if (auto registered = ProfileSettingsSection::RegisterIdentity(ui_state_); !registered) {
      ReportFailure(registered.error());
      return;
    }
    const char* message =
        renewing ? "Registration renewed — Brief API key updated" : "Registered — Brief API key saved";
    status_ = message;
    PushUiStateToBindings();
    DirtyAll();
    UserFeedback::Ok(message);
  });
}

void SettingsController::OnRotateBriefLlmKey() {
  PullBindingsToUiState();
  PinGateController::Instance().EnsureUnlocked([this](const bool unlocked) {
    if (!unlocked) {
      ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to rotate API key"));
      return;
    }
    if (auto rotated = ProfileSettingsSection::RotateBriefLlmKey(ui_state_); !rotated) {
      ReportFailure(rotated.error());
      return;
    }
    status_ = "Brief API key rotated";
    PushUiStateToBindings();
    DirtyAll();
    UserFeedback::Ok("Brief API key rotated");
  });
}

void SettingsController::OnCopyProfileId() {
  const std::string peer_id = bindings_.profile_peer_id.c_str();
  if (peer_id.empty()) {
    UserFeedback::Fail("No Peer ID yet — register on the network first.");
    return;
  }
  if (Rml::SystemInterface* system = Rml::GetSystemInterface()) {
    system->SetClipboardText(bindings_.profile_peer_id);
  }
  UserFeedback::Ok("Peer ID copied");
}

void SettingsController::OnShareProfile() {
  const std::string peer_id = bindings_.profile_peer_id.c_str();
  if (peer_id.empty()) {
    UserFeedback::Fail("No Peer ID yet — register on the network first.");
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
  UserFeedback::Ok("Invite copied");
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

void SettingsController::OnResetProfileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnResetProfile();
}

void SettingsController::OnResetProfile() {
  ShellFeedback::ShowConfirmWithCheckbox(
      ShellHost::Instance().State(), Tr("settings.storage.reset_confirm_title"),
      Tr("settings.storage.reset_confirm_message"), Tr("settings.storage.reset_confirm_check"), false,
      [this](const bool confirmed, const bool checked) {
        if (!confirmed) {
          return;
        }
        if (!checked) {
          UserFeedback::Fail(Tr("settings.storage.reset_confirm_check"));
          return;
        }
        PerformResetProfile();
      });
  ShellHost::Instance().RequestSyncLayout();
}

void SettingsController::PerformResetProfile() {
  const BootstrapResult& bootstrap = SessionStore::Instance().Snapshot();
  const std::string profile_dir = bootstrap.profile_data_dir;
  const AppConfig config = bootstrap.config;

  if (profile_dir.empty()) {
    ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile path unavailable"));
    return;
  }

  log().info << "Resetting profile data at " << profile_dir;

  MessagingHub::Instance().Shutdown();
  ProfileSecretsService::Instance().Shutdown();

  std::error_code ec;
  std::filesystem::remove_all(profile_dir, ec);
  if (ec) {
    log().error << "remove_all(" << profile_dir << "): " << ec.message();
    ReportFailure(AppError::Storage(Err::Storage::Failed, "Failed to delete profile data: " + ec.message()));
    return;
  }

  AppPaths::EnsureDirs(profile_dir);
  if (auto manifest = SchemaVersion::EnsureProfileManifest(profile_dir); !manifest) {
    ReportFailure(manifest.error());
    return;
  }

  if (auto secrets = ProfileSecretsService::Instance().Initialize(profile_dir); !secrets) {
    ReportFailure(secrets.error());
    return;
  }

  if (auto hub = MessagingHub::Instance().Initialize(config, profile_dir); !hub) {
    ReportFailure(hub.error());
    return;
  }

  if (auto prefs = SessionStore::Instance().ReloadProfilePrefs(); !prefs) {
    ReportFailure(prefs.error());
    return;
  }

  if (ChatSessionActions::Instance().on_profile_data_reset) {
    ChatSessionActions::Instance().on_profile_data_reset();
  }
  ContactsController::Instance().Refresh();

  bindings_.pin_change_old = "";
  bindings_.pin_change_new = "";
  bindings_.pin_change_confirm = "";
  status_ = "";
  SyncBindingsFromSession();
  DirtyAll();
  UserFeedback::Ok(Tr("settings.storage.profile_reset"));
  ShellHost::Instance().RequestSyncLayout();
}

void SettingsController::OnChangePin() {
  if (!ProfileSecretsService::Instance().IsInitialized() || !ProfileSecretsService::Instance().HasVault()) {
    ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Set up key protection first")
                      .WithUser("Set up key protection first"));
    return;
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    PinGateController::Instance().EnsureUnlocked([this](const bool unlocked) {
      if (!unlocked) {
        ReportFailure(AppError::Pin(Err::Pin::Required, "Unlock profile PIN to change it"));
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
    ReportFailure(AppError::Pin(Err::Pin::Required, "Current and new PIN are required")
                      .WithUser("Current and new PIN are required"));
    return;
  }
  if (new_pin != confirm) {
    ReportFailure(AppError::Pin(Err::Pin::Mismatch, "New PINs do not match"));
    return;
  }
  if (new_pin.size() < 4) {
    ReportFailure(AppError::Pin(Err::Pin::TooShort, "Use at least 4 characters"));
    return;
  }

  DataKeyVault* vault = ProfileSecretsService::Instance().Vault();
  if (vault == nullptr) {
    ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Vault unavailable"));
    return;
  }
  if (auto changed = vault->ChangePin(old_pin, new_pin); !changed) {
    ReportFailure(changed.error());
    return;
  }

  ProfilePreferences prefs = SessionStore::Instance().Snapshot().profile_prefs;
  prefs.pin_is_default = false;
  if (auto saved = SessionStore::Instance().SaveProfilePrefs(prefs); !saved) {
    ReportFailure(saved.error());
    return;
  }

  bindings_.pin_change_old = "";
  bindings_.pin_change_new = "";
  bindings_.pin_change_confirm = "";
  status_ = "PIN updated";
  SyncBindingsFromSession();
  DirtyAll();
  UserFeedback::Ok("PIN updated");
}

} // namespace pbr
