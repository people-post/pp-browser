#include <stdexcept>
#include "feature/ui/SettingsController.h"

#include "base/crypto/ProfileSecretsService.h"
#include "base/data/AppPaths.h"
#include "base/data/LlmPreset.h"
#include "base/data/SchemaVersion.h"
#include "base/data/SessionStore.h"
#include "base/i18n/LocalizationService.h"
#include "base/platform/BrowserThread.h"
#include "base/ui/ContextMenuHost.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/SettingsPortsViews.h"
#include "feature/ui/DataModelHost.h"
#include "base/crypto/ProfileUnlockGate.h"
#include "feature/ui/ProfileSettingsSection.h"
#include "feature/ui/SecuritySettingsSection.h"
#include "feature/ui/UiEditSession.h"
#include "feature/ui/UserFeedback.h"
#include "base/error/AppError.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>
#include <SDL3/SDL.h>

#include <filesystem>

namespace pbr {

SettingsController* SettingsController::installed_instance_ = nullptr;

void SettingsController::InstallInstance(SettingsController& controller) {
  installed_instance_ = &controller;
}

void SettingsController::ClearInstance() {
  installed_instance_ = nullptr;
}

SettingsController& SettingsController::Instance() {
  if (!installed_instance_) {
    throw std::runtime_error("SettingsController not installed");
  }
  return *installed_instance_;
}

namespace {

constexpr uint64_t kDebounceMs = 500;
constexpr uint64_t kToastSuppressMs = 2000;

Rml::String EventValue(Rml::Event& ev) {
  return ev.GetParameter<Rml::String>("value", Rml::String());
}

std::string ReachabilityStatusLabel(SettingsReachabilityView::Status status) {
  switch (status) {
  case SettingsReachabilityView::Status::Checking:
    return Tr("settings.network.reachability.checking");
  case SettingsReachabilityView::Status::Reachable:
    return Tr("settings.network.reachability.reachable");
  case SettingsReachabilityView::Status::OutboundOnly:
    return Tr("settings.network.reachability.outbound_only");
  case SettingsReachabilityView::Status::Blocked:
    return Tr("settings.network.reachability.blocked");
  case SettingsReachabilityView::Status::Unknown:
  default:
    return Tr("settings.network.reachability.unknown");
  }
}

std::string ReachabilitySummary(const SettingsReachabilityView& view) {
  switch (view.status) {
  case SettingsReachabilityView::Status::Checking:
    return Tr("settings.network.reachability.summary_checking");
  case SettingsReachabilityView::Status::Reachable:
    if (view.has_global_ipv6 && view.dial_back_ok) {
      return Tr("settings.network.reachability.summary_reachable_ipv6");
    }
    if (view.upnp_mapped) {
      return Tr("settings.network.reachability.summary_reachable_upnp");
    }
    return Tr("settings.network.reachability.summary_reachable");
  case SettingsReachabilityView::Status::OutboundOnly:
    return Tr("settings.network.reachability.summary_outbound_only");
  case SettingsReachabilityView::Status::Blocked:
    return Tr("settings.network.reachability.summary_blocked");
  case SettingsReachabilityView::Status::Unknown:
  default:
    return Tr("settings.network.reachability.summary_unknown");
  }
}

/** Anchor ShowActions float menus under the right side of a settings choice row. */
Rml::Vector2i ChoiceRowMenuPosition(Rml::Event& ev) {
  Rml::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  if (!target) {
    return {0, 0};
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

  // Match .context-menu-panel min-width so the menu's right edge lines up with the value.
  return MenuPositionBelowRightAligned(anchor);
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
  // Section titles use Tr(); catalogs load later in Application::Initialize.
  // Application calls RefreshLocalizedChrome() after LoadFromAssets + BindCommands.
}

void SettingsController::BindCommands(SettingsCommands commands) {
  commands_ = std::move(commands);
  if (auto* profile = dynamic_cast<ProfileSettingsSection*>(FindHandler("profile"))) {
    profile->BindPorts(&commands_);
  }
  if (auto* appearance = dynamic_cast<AppearanceSettingsSection*>(FindHandler("appearance"))) {
    appearance->BindPorts(&commands_);
  }
  if (auto* security = dynamic_cast<SecuritySettingsSection*>(FindHandler("security"))) {
    security->BindPorts(&commands_);
  }
}

void SettingsController::BindShellNavigation(ShellNavigationPorts ports) {
  shell_navigation_ = std::move(ports);
}

void SettingsController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

ShellChromeSnapshot SettingsController::ChromeSnapshot() const {
  if (shell_navigation_.snapshot) {
    return shell_navigation_.snapshot();
  }
  return {};
}

void SettingsController::BindUnlockGate(ProfileUnlockGate& unlock_gate) {
  unlock_gate_ = &unlock_gate;
}

SettingsCommands& SettingsController::Commands() {
  return commands_;
}

const SettingsCommands& SettingsController::Commands() const {
  return commands_;
}

SessionStore& SettingsController::Store() {
  if (!commands_.session_store) {
    throw std::runtime_error("SettingsController session_store port not bound");
  }
  return commands_.session_store();
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
  ui_state_.node_enabled = bindings_.node_enabled.c_str();
  ui_state_.show_node_toggle = bindings_.show_node_toggle;
  ui_state_.libp2p_listen_multiaddr = bindings_.libp2p_listen_multiaddr.c_str();
  ui_state_.libp2p_status_message = bindings_.libp2p_status_message.c_str();
  ui_state_.reachability_status_label = bindings_.reachability_status_label.c_str();
  ui_state_.reachability_summary = bindings_.reachability_summary.c_str();
  ui_state_.reachability_help_kind = bindings_.reachability_help_kind.c_str();
  ui_state_.show_connection_card = bindings_.show_connection_card;
  ui_state_.show_reachability_help = bindings_.show_reachability_help;
  ui_state_.circuit_relay_enabled = bindings_.circuit_relay_enabled.c_str();
  ui_state_.show_circuit_relay_toggle = bindings_.show_circuit_relay_toggle;
  ui_state_.media_relay_enabled = bindings_.media_relay_enabled.c_str();
  ui_state_.show_media_relay_toggle = bindings_.show_media_relay_toggle;
  ui_state_.prefer_contacts_for_routing = bindings_.prefer_contacts_for_routing.c_str();
  ui_state_.show_prefer_contacts_toggle = bindings_.show_prefer_contacts_toggle;
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
  bindings_.node_enabled = ui_state_.node_enabled.c_str();
  bindings_.show_node_toggle = ui_state_.show_node_toggle;
  bindings_.libp2p_listen_multiaddr = ui_state_.libp2p_listen_multiaddr.c_str();
  bindings_.libp2p_status_message = ui_state_.libp2p_status_message.c_str();
  bindings_.reachability_status_label = ui_state_.reachability_status_label.c_str();
  bindings_.reachability_summary = ui_state_.reachability_summary.c_str();
  bindings_.reachability_help_kind = ui_state_.reachability_help_kind.c_str();
  bindings_.show_connection_card = ui_state_.show_connection_card;
  bindings_.show_reachability_help = ui_state_.show_reachability_help;
  bindings_.circuit_relay_enabled = ui_state_.circuit_relay_enabled.c_str();
  bindings_.show_circuit_relay_toggle = ui_state_.show_circuit_relay_toggle;
  bindings_.media_relay_enabled = ui_state_.media_relay_enabled.c_str();
  bindings_.show_media_relay_toggle = ui_state_.show_media_relay_toggle;
  bindings_.prefer_contacts_for_routing = ui_state_.prefer_contacts_for_routing.c_str();
  bindings_.show_prefer_contacts_toggle = ui_state_.show_prefer_contacts_toggle;
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
  const BootstrapResult& bootstrap = Store().Snapshot();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    handler->SyncFromSession(bootstrap, ui_state_);
  }
  if (commands_.last_libp2p_error) {
    ui_state_.libp2p_status_message = commands_.last_libp2p_error();
  } else {
    ui_state_.libp2p_status_message.clear();
  }
  ApplyReachability();
  // Baseline for blur commit.
  // Sync+DirtyAll would SetValue the input and reset cursor / feel like focus loss.
  auto& edits = UiEditSession::Instance();
  const std::string live = bindings_.profile_nickname.c_str();
  if (commands_.load_profile_identity) {
    const ProfileIdentityView view = commands_.load_profile_identity();
    if (view.ready) {
      // Resolve against the *previous* baseline first. OnLoaded before Resolve makes an
      // empty binding look mid-edit vs the freshly loaded nickname (keeps nickname blank).
      ui_state_.profile_nickname =
          edits.ResolveAfterLoad(kUiFieldProfileNickname, view.nickname, live);
      if (!edits.IsMidEdit(kUiFieldProfileNickname, live)) {
        edits.OnLoaded(kUiFieldProfileNickname, view.nickname);
      }
    }
  } else if (const std::string* baseline = edits.Baseline(kUiFieldProfileNickname)) {
    ui_state_.profile_nickname =
        edits.ResolveAfterLoad(kUiFieldProfileNickname, *baseline, live);
  }
  PushUiStateToBindings();
}

void SettingsController::ReloadFromDisk() {
  if (!commands_.reload_from_disk) {
    log().warning << "ReloadFromDisk: reload_from_disk port not bound";
    return;
  }
  if (auto reloaded = commands_.reload_from_disk(); !reloaded) {
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

  return DataModelHost::Instance().Register(context, "settings", [this](Rml::DataModelConstructor& ctor) {
    auto& controller = *this;
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
    ctor.Bind("node_enabled", &controller.bindings_.node_enabled);
    ctor.Bind("show_node_toggle", &controller.bindings_.show_node_toggle);
    ctor.Bind("libp2p_listen_multiaddr", &controller.bindings_.libp2p_listen_multiaddr);
    ctor.Bind("libp2p_status_message", &controller.bindings_.libp2p_status_message);
    ctor.Bind("reachability_status_label", &controller.bindings_.reachability_status_label);
    ctor.Bind("reachability_summary", &controller.bindings_.reachability_summary);
    ctor.Bind("reachability_help_kind", &controller.bindings_.reachability_help_kind);
    ctor.Bind("show_connection_card", &controller.bindings_.show_connection_card);
    ctor.Bind("show_reachability_help", &controller.bindings_.show_reachability_help);
    ctor.Bind("circuit_relay_enabled", &controller.bindings_.circuit_relay_enabled);
    ctor.Bind("show_circuit_relay_toggle", &controller.bindings_.show_circuit_relay_toggle);
    ctor.Bind("media_relay_enabled", &controller.bindings_.media_relay_enabled);
    ctor.Bind("show_media_relay_toggle", &controller.bindings_.show_media_relay_toggle);
    ctor.Bind("prefer_contacts_for_routing", &controller.bindings_.prefer_contacts_for_routing);
    ctor.Bind("show_prefer_contacts_toggle", &controller.bindings_.show_prefer_contacts_toggle);
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
    ctor.BindEventCallback("toggle_node_enabled", &SettingsController::ToggleNodeEnabledCallback);
    ctor.BindEventCallback("retest_reachability", &SettingsController::RetestReachabilityCallback);
    ctor.BindEventCallback("try_upnp_port", &SettingsController::TryUpnpPortCallback);
    ctor.BindEventCallback("show_reachability_help", &SettingsController::ShowReachabilityHelpCallback);
    ctor.BindEventCallback("dismiss_reachability_help", &SettingsController::DismissReachabilityHelpCallback);
    ctor.BindEventCallback("toggle_circuit_relay", &SettingsController::ToggleCircuitRelayCallback);
    ctor.BindEventCallback("toggle_media_relay", &SettingsController::ToggleMediaRelayCallback);
    ctor.BindEventCallback("toggle_prefer_contacts", &SettingsController::TogglePreferContactsCallback);
    ctor.BindEventCallback("on_profile_nickname_commit", &SettingsController::OnProfileNicknameCommitCallback);
    ctor.BindEventCallback("register_profile", &SettingsController::OnRegisterProfileCallback);
    ctor.BindEventCallback("rotate_brief_llm_key", &SettingsController::OnRotateBriefLlmKeyCallback);
    ctor.BindEventCallback("copy_profile_id", &SettingsController::OnCopyProfileIdCallback);
    ctor.BindEventCallback("share_profile", &SettingsController::OnShareProfileCallback);
    ctor.BindEventCallback("add_mcp_server", &SettingsController::OnAddMcpServerCallback);
    ctor.BindEventCallback("remove_mcp_server", &SettingsController::OnRemoveMcpServerCallback);
    ctor.BindEventCallback("change_pin", &SettingsController::OnChangePinCallback);
    ctor.BindEventCallback("clear_undelivered_older_than", &SettingsController::OnClearUndeliveredCallback);
    ctor.BindEventCallback("reset_profile", &SettingsController::OnResetProfileCallback);
  });
}

void SettingsController::DirtyAll(bool include_profile_nickname) {
  auto& host = DataModelHost::Instance();
  const std::string live = bindings_.profile_nickname.c_str();
  const bool push_nick =
      include_profile_nickname &&
      UiEditSession::Instance().ShouldPushToView(kUiFieldProfileNickname, live);
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
  host.Dirty("settings", "node_enabled");
  host.Dirty("settings", "show_node_toggle");
  host.Dirty("settings", "libp2p_listen_multiaddr");
  host.Dirty("settings", "libp2p_status_message");
  host.Dirty("settings", "reachability_status_label");
  host.Dirty("settings", "reachability_summary");
  host.Dirty("settings", "reachability_help_kind");
  host.Dirty("settings", "show_connection_card");
  host.Dirty("settings", "show_reachability_help");
  host.Dirty("settings", "circuit_relay_enabled");
  host.Dirty("settings", "show_circuit_relay_toggle");
  host.Dirty("settings", "media_relay_enabled");
  host.Dirty("settings", "show_media_relay_toggle");
  host.Dirty("settings", "prefer_contacts_for_routing");
  host.Dirty("settings", "show_prefer_contacts_toggle");
  if (push_nick) {
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
  SyncBindingsFromSession();
  DirtyAll();
  if (context_) {
    context_->Update();
  }
  // Select widgets can emit a spurious change on the frame after remount.
  BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
    const ShellChromeSnapshot chrome = ChromeSnapshot();
    if (chrome.nav_tab != NavTab::Me && !chrome.account_sheet_open) {
      suppress_auto_save_ = false;
      UiEditSession::Instance().EndRemount();
      return;
    }
    SyncBindingsFromSession();
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    suppress_auto_save_ = false;
    UiEditSession::Instance().EndRemount();
  });
}

void SettingsController::OnShellLayoutSynced() {
  if (!suppress_auto_save_) {
    return;
  }
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (chrome.nav_tab == NavTab::Me || chrome.account_sheet_open) {
    FinishPaneResync();
  } else {
    suppress_auto_save_ = false;
    UiEditSession::Instance().EndRemount();
  }
}

void SettingsController::OnNavTabActivated() {
  log().info << "OnNavTabActivated";
  // Load only — never flush on activate (bindings may still be empty).
  dirty_sections_.clear();
  debounce_deadline_ms_ = 0;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  in_account_sheet_ = false;
  if (shell_navigation_.clear_local_back) {
    shell_navigation_.clear_local_back("settings_detail");
  }
  compact_layout_ = ChromeSnapshot().layout_mode == LayoutMode::Compact;
  // Remount gate must wrap Reload/Sync: an empty nickname binding vs a prior baseline
  // would otherwise look mid-edit and keep the field blank (then blur-commit wipes it).
  suppress_auto_save_ = true;
  UiEditSession::Instance().BeginRemount();
  ReloadFromDisk();
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
  if (shell_navigation_.clear_local_back) {
    shell_navigation_.clear_local_back("settings_detail");
  }
  in_account_sheet_ = true;
  suppress_auto_save_ = true;
  UiEditSession::Instance().BeginRemount();
  ReloadFromDisk();
}

void SettingsController::OnMeSurfaceClosed() {
  CommitProfileNickname(/*show_toast=*/false);
  FlushPending();
}

void SettingsController::OnAccountSheetClosed() {
  OnMeSurfaceClosed();
  in_account_sheet_ = false;
  show_detail_ = false;
  selected_id_.clear();
  selected_title_.clear();
  if (shell_navigation_.clear_local_back) {
    shell_navigation_.clear_local_back("settings_detail");
  }
}

void SettingsController::SyncLayoutMode() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  const bool compact = chrome.layout_mode == LayoutMode::Compact;
  if (compact_layout_ == compact) {
    return;
  }
  compact_layout_ = compact;

  const Rml::String saved_id = selected_id_;
  const Rml::String saved_title = selected_title_;
  const bool had_detail = !saved_id.empty() || show_detail_;

  if (compact) {
    if (chrome.nav_tab == NavTab::Me) {
      if (shell_navigation_.clear_primary_pane) {
        shell_navigation_.clear_primary_pane();
      }
      if (shell_navigation_.select_nav_tab) {
        shell_navigation_.select_nav_tab(NavTab::Home);
      }
      selected_id_ = saved_id;
      selected_title_ = saved_title;
      show_detail_ = had_detail;
      if (shell_navigation_.open_account_sheet) {
        shell_navigation_.open_account_sheet();
      }
      // OpenAccountSheet resets selection; restore sheet detail state.
      selected_id_ = saved_id;
      selected_title_ = saved_title;
      show_detail_ = had_detail;
      in_account_sheet_ = true;
      if (had_detail) {
        if (shell_navigation_.clear_local_back) {
          shell_navigation_.clear_local_back("settings_detail");
        }
        if (shell_navigation_.push_local_back) {
          shell_navigation_.push_local_back("settings_detail", [this] { ApplyBackToListUi(); });
        }
      }
      DirtyAll();
      return;
    }
  } else if (chrome.account_sheet_open) {
    if (shell_navigation_.close_account_sheet) {
      shell_navigation_.close_account_sheet();
    }
    selected_id_ = saved_id;
    selected_title_ = saved_title;
    show_detail_ = false;
    in_account_sheet_ = false;
    if (shell_navigation_.select_nav_tab) {
      shell_navigation_.select_nav_tab(NavTab::Me);
    }
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
  if (suppress_auto_save_ || UiEditSession::Instance().RemountBlocking()) {
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
  if (suppress_auto_save_ || UiEditSession::Instance().RemountBlocking()) {
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
  if (auto flushed = handler->Flush(ui_state_, Store()); !flushed) {
    log().warning << "FlushSection(" << section_id << ") failed: " << AppError::Log(flushed.error());
    ReportFailure(flushed.error());
    return false;
  }

  dirty_sections_.erase(section_id);
  if (dirty_sections_.empty()) {
    debounce_deadline_ms_ = 0;
  }

  status_ = "";
  if (section_id == "profile" && commands_.load_profile_identity) {
    const ProfileIdentityView view = commands_.load_profile_identity();
    if (view.ready) {
      UiEditSession::Instance().OnCommitted(kUiFieldProfileNickname, view.nickname);
      ui_state_.profile_nickname = view.nickname;
    }
  }
  PushUiStateToBindings();

  // Field commits must not DirtyAll/DirtyWindow: SetValue/remount re-enters blur and
  // looks like per-keystroke focus loss. Toast paths still refresh chrome.
  if (section_id == "profile" && !show_toast) {
    return true;
  }

  DirtyAll();
  if (show_toast) {
    MaybeShowSaveToast(section_id);
  }
  if (shell_navigation_.dirty_window) {
    shell_navigation_.dirty_window();
  }
  return true;
}

void SettingsController::CommitProfileNickname(bool show_toast) {
  if (suppress_auto_save_ || UiEditSession::Instance().RemountBlocking()) {
    return;
  }
  const std::string live = bindings_.profile_nickname.c_str();
  if (!UiEditSession::Instance().ShouldCommit(kUiFieldProfileNickname, live)) {
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

  if (!shell_feedback_.show_confirm) {
    return;
  }
  shell_feedback_.show_confirm(
      Tr("settings.reset_defaults_confirm_title"), Tr("settings.reset_defaults_confirm_message"),
      [this, section_id](const bool ok) {
        if (!ok) {
          return;
        }
        PerformResetSection(section_id);
      });
}

void SettingsController::PerformResetSection(const std::string& section_id) {
  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  handler->ResetToDefaults(ui_state_, Store());
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
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  const bool sheet = chrome.account_sheet_open || in_account_sheet_;
  if (sheet) {
    show_detail_ = true;
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    if (shell_navigation_.push_local_back) {
      shell_navigation_.push_local_back("settings_detail", [this] { ApplyBackToListUi(); });
    }
    BrowserThread::PostTask(BrowserThreadId::UI, [this]() {
      suppress_auto_save_ = true;
      UiEditSession::Instance().BeginRemount();
      FinishPaneResync();
      if (shell_navigation_.refresh_dismiss_gestures) {
        shell_navigation_.refresh_dismiss_gestures();
      }
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
    UiEditSession::Instance().BeginRemount();
    FinishPaneResync();
    log().info << "OnSelectSection (pane) complete id=" << selected_id_.c_str();
  });
}

void SettingsController::OpenSettingsDetailPane() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  compact_layout_ = chrome.layout_mode == LayoutMode::Compact;
  // Compact Me uses the account sheet, not transient panes.
  if (compact_layout_ || chrome.account_sheet_open) {
    return;
  }

  if (chrome.settings_detail_transient) {
    if (shell_navigation_.pop_transient) {
      shell_navigation_.pop_transient();
    }
  }
  if (shell_navigation_.set_primary_pane) {
    shell_navigation_.set_primary_pane("settings_detail");
  }
}

bool SettingsController::CloseSettingsDetailPane() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (chrome.settings_detail_transient) {
    if (shell_navigation_.pop_transient) {
      shell_navigation_.pop_transient();
    }
    return true;
  }
  if (chrome.layout_mode != LayoutMode::Compact && chrome.nav_tab == NavTab::Me) {
    if (shell_navigation_.clear_primary_pane) {
      shell_navigation_.clear_primary_pane();
    }
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
    UiEditSession::Instance().BeginRemount();
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    suppress_auto_save_ = false;
    UiEditSession::Instance().EndRemount();
    if (shell_navigation_.refresh_dismiss_gestures) {
      shell_navigation_.refresh_dismiss_gestures();
    }
  });
}

void SettingsController::OnBackToList() {
  log().info << "OnBackToList";
  FlushPending();
  if (shell_navigation_.has_local_back && shell_navigation_.has_local_back("settings_detail")) {
    if (shell_navigation_.request_dismiss_instant) {
      shell_navigation_.request_dismiss_instant();
    }
    return;
  }
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (in_account_sheet_ || chrome.account_sheet_open) {
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
  if (handler->IsPersisted(controller.ui_state_, controller.Store().Snapshot())) {
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
  // Live apply via app port; SessionStore → ConfigApplyBridge re-applies after flush.
  if (commands_.apply_appearance) {
    commands_.apply_appearance(appearance_pref);
  }
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

  if (!commands_.language_display_label || !commands_.available_locales) {
    log().warning << "OnChooseLanguage: locale ports not bound";
    return;
  }

  const std::string current = bindings_.language.empty() ? "system" : std::string(bindings_.language.c_str());
  const auto label_for = [this](const std::string& pref) {
    return commands_.language_display_label(pref);
  };

  std::vector<ContextMenuAction> actions;
  actions.push_back({.id = "system",
                     .label = label_for("system"),
                     .enabled = {},
                     .run =
                         [this]() {
                           ApplyLanguageChoice("system");
                         },
                     .icon = {},
                     .danger = false,
                     .selected = current == "system"});

  for (const LocaleInfo& info : commands_.available_locales()) {
    const std::string tag = info.tag;
    actions.push_back({.id = tag,
                       .label = label_for(tag),
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
  if (commands_.language_display_label) {
    bindings_.language_label = commands_.language_display_label(language_pref).c_str();
  } else {
    bindings_.language_label = language_pref.c_str();
  }
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

void SettingsController::ApplyReachability() {
  const bool messaging_ready = commands_.messaging_ready && commands_.messaging_ready();
  ui_state_.show_connection_card =
      ui_state_.show_node_toggle && ui_state_.node_enabled == "on" && messaging_ready;
  ui_state_.show_circuit_relay_toggle = ui_state_.show_connection_card;
  ui_state_.show_media_relay_toggle = ui_state_.show_connection_card;
  ui_state_.show_prefer_contacts_toggle = ui_state_.show_connection_card;

  if (ui_state_.show_connection_card && commands_.load_reachability) {
    const SettingsReachabilityView view = commands_.load_reachability();
    ui_state_.reachability_status_label = ReachabilityStatusLabel(view.status);
    ui_state_.reachability_summary = ReachabilitySummary(view);
    ui_state_.reachability_help_kind = view.help_kind;
  } else {
    ui_state_.reachability_status_label.clear();
    ui_state_.reachability_summary.clear();
    ui_state_.reachability_help_kind.clear();
    ui_state_.show_reachability_help = false;
  }

  if (commands_.session_store) {
    const auto& cfg = Store().Snapshot().config.libp2p;
    ui_state_.circuit_relay_enabled = cfg.capabilities.circuit_relay ? "on" : "off";
    ui_state_.media_relay_enabled = cfg.capabilities.media_relay ? "on" : "off";
    ui_state_.prefer_contacts_for_routing = cfg.prefer_contacts_for_routing ? "on" : "off";
  }
  PushUiStateToBindings();
}

void SettingsController::SyncReachability() {
  ApplyReachability();
  DirtyAll();
}

void SettingsController::ToggleNodeEnabledCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                   const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (!controller.bindings_.show_node_toggle) {
    return;
  }
  controller.bindings_.node_enabled = controller.bindings_.node_enabled == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("network");
  controller.DirtyAll();
}

void SettingsController::RetestReachabilityCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                    const Rml::VariantList& /*args*/) {
  if (Instance().Commands().run_reachability_probe) {
    Instance().Commands().run_reachability_probe(false);
  }
  Instance().SyncReachability();
}

void SettingsController::TryUpnpPortCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                             const Rml::VariantList& /*args*/) {
  if (Instance().Commands().try_upnp_port_mapping) {
    Instance().Commands().try_upnp_port_mapping();
  }
  Instance().SyncReachability();
}

void SettingsController::ShowReachabilityHelpCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                      const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.bindings_.show_reachability_help = true;
  controller.PullBindingsToUiState();
  controller.DirtyAll();
}

void SettingsController::DismissReachabilityHelpCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                         const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  controller.bindings_.show_reachability_help = false;
  controller.PullBindingsToUiState();
  controller.DirtyAll();
}

void SettingsController::ToggleCircuitRelayCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                    const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (!controller.bindings_.show_circuit_relay_toggle) {
    return;
  }
  controller.bindings_.circuit_relay_enabled =
      controller.bindings_.circuit_relay_enabled == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("network");
  controller.DirtyAll();
}

void SettingsController::ToggleMediaRelayCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                  const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (!controller.bindings_.show_media_relay_toggle) {
    return;
  }
  controller.bindings_.media_relay_enabled =
      controller.bindings_.media_relay_enabled == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("network");
  controller.DirtyAll();
}

void SettingsController::TogglePreferContactsCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                      const Rml::VariantList& /*args*/) {
  auto& controller = Instance();
  if (!controller.bindings_.show_prefer_contacts_toggle) {
    return;
  }
  controller.bindings_.prefer_contacts_for_routing =
      controller.bindings_.prefer_contacts_for_routing == "on" ? "off" : "on";
  controller.PullBindingsToUiState();
  controller.MarkSectionDirty("network");
  controller.DirtyAll();
}

void SettingsController::OnProfileNicknameCommitCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                         const Rml::VariantList& /*args*/) {
  // Persist on blur only when the field differs from last loaded/saved nickname.
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
  CommitProfileNickname(/*show_toast=*/false);
  PullBindingsToUiState();
  const bool renewing = ui_state_.profile_registered == "yes";
  if (!unlock_gate_) {
    ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to register"));
    return;
  }
  unlock_gate_->EnsureUnlocked([this, renewing](const bool unlocked) {
    if (!unlocked) {
      ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to register"));
      return;
    }
    if (!commands_.register_identity) {
      ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Registration is not available"));
      return;
    }
    if (auto registered = commands_.register_identity({.nickname = ui_state_.profile_nickname}); !registered) {
      ReportFailure(registered.error());
      return;
    }
    if (commands_.load_profile_identity) {
      const ProfileIdentityView view = commands_.load_profile_identity();
      if (view.ready) {
        UiEditSession::Instance().OnCommitted(kUiFieldProfileNickname, view.nickname);
      }
    }
    const char* message =
        renewing ? "Registration renewed — Brief API key updated" : "Registered — Brief API key saved";
    status_ = message;
    SyncBindingsFromSession();
    DirtyAll();
    UserFeedback::Ok(message);
  });
}

void SettingsController::OnRotateBriefLlmKey() {
  PullBindingsToUiState();
  if (!unlock_gate_) {
    ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to rotate API key"));
    return;
  }
  unlock_gate_->EnsureUnlocked([this](const bool unlocked) {
    if (!unlocked) {
      ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required to rotate API key"));
      return;
    }
    if (!commands_.rotate_brief_llm_key) {
      ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Key rotation is not available"));
      return;
    }
    if (auto rotated = commands_.rotate_brief_llm_key(); !rotated) {
      // Identity may have been marked expired on 401/403.
      SyncBindingsFromSession();
      DirtyAll();
      ReportFailure(rotated.error());
      return;
    }
    status_ = "Brief API key rotated";
    SyncBindingsFromSession();
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

void SettingsController::OnClearUndeliveredCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                    const Rml::VariantList& /*args*/) {
  Instance().OnClearUndeliveredOlderThan();
}

void SettingsController::OnClearUndeliveredOlderThan() {
  if (!commands_.clear_undelivered_older_than) {
    ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.clear_undelivered.not_ready")));
    return;
  }

  if (!shell_feedback_.show_confirm) {
    return;
  }
  shell_feedback_.show_confirm(
      Tr("settings.security.clear_undelivered_confirm_title"),
      Tr("settings.security.clear_undelivered_confirm_message"),
      [this](const bool ok) {
        if (!ok) {
          return;
        }
        BrowserThread::PostTask(BrowserThreadId::IO, [this]() {
          auto result = commands_.clear_undelivered_older_than
                            ? commands_.clear_undelivered_older_than(7)
                            : Roe<void>{Error("Messaging is not ready")};
          BrowserThread::PostTask(BrowserThreadId::UI, [this, result = std::move(result)]() mutable {
            if (!result) {
              ReportFailure(result.error());
              return;
            }
            UserFeedback::Ok(Tr("settings.security.clear_undelivered_done"));
            status_ = "";
            DirtyAll();
          });
        });
      });
}

void SettingsController::OnResetProfileCallback(Rml::DataModelHandle /*model*/, Rml::Event& /*ev*/,
                                                const Rml::VariantList& /*args*/) {
  Instance().OnResetProfile();
}

void SettingsController::OnResetProfile() {
  if (!shell_feedback_.show_confirm_with_checkbox) {
    return;
  }
  shell_feedback_.show_confirm_with_checkbox(
      Tr("settings.storage.reset_confirm_title"), Tr("settings.storage.reset_confirm_message"),
      Tr("settings.storage.reset_confirm_check"), false,
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
}

void SettingsController::PerformResetProfile() {
  if (!commands_.reset_active_profile) {
    ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile reset is not available"));
    return;
  }

  log().info << "Requesting active profile reset";
  if (auto reset = commands_.reset_active_profile(); !reset) {
    ReportFailure(reset.error());
    return;
  }

  bindings_.pin_change_old = "";
  bindings_.pin_change_new = "";
  bindings_.pin_change_confirm = "";
  status_ = "";
  SyncBindingsFromSession();
  DirtyAll();
  UserFeedback::Ok(Tr("settings.storage.profile_reset"));
  if (shell_navigation_.request_sync_layout) {
    shell_navigation_.request_sync_layout(/*restore_focus_after=*/false, nullptr);
  }
}

void SettingsController::OnChangePin() {
  if (!ProfileSecretsService::Instance().IsInitialized() || !ProfileSecretsService::Instance().HasVault()) {
    ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Set up key protection first")
                      .WithUser("Set up key protection first"));
    return;
  }
  if (!ProfileSecretsService::Instance().IsUnlocked()) {
    if (!unlock_gate_) {
      ReportFailure(AppError::Pin(Err::Pin::Required, "Unlock profile PIN to change it"));
      return;
    }
    unlock_gate_->EnsureUnlocked([this](const bool unlocked) {
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

  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  prefs.pin_is_default = false;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
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
