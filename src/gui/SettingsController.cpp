#include <stdexcept>
#include "gui/SettingsController.h"

#include "lib/ui/DataEvent.h"
#include "foundation/platform/NativeFileDialog.h"

#include "foundation/data/AppPaths.h"
#include "foundation/data/LlmPreset.h"
#include "foundation/data/SchemaVersion.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/net/ClientCompat.h"
#include "foundation/runtime/AppRuntime.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/ViewCatalog.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/ReachabilityNudge.h"
#include "feature/settings/StorageSettingsSection.h"
#include "feature/settings/SettingsPortsViews.h"
#include "gui/shell/DataModelHost.h"
#include "gui/ProfileSettingsSection.h"
#include "gui/shell/RmlMount.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/SecuritySettingsSection.h"
#include "lib/ui/UiEditSession.h"
#include "gui/UserFeedback.h"
#include "gui/BlobQuotaRecoveryFlow.h"
#include "foundation/error/AppError.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/PlatformShare.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "gui/AccountInvite.h"

#include <ui/dom/Context.h>
#include <ui/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/base/SystemInterface.h>
#include <SDL3/SDL.h>

#include <filesystem>
#include <functional>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

constexpr uint64_t kDebounceMs = 500;
constexpr uint64_t kToastSuppressMs = 2000;

ui::String EventValue(ui::Event& ev) {
  return ev.GetParameter<ui::String>("value", ui::String());
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

std::string DirectConnectionsDisplayLabel(const std::string& audience) {
  const auto parsed = mesh::DirectAudienceFromName(audience);
  return Tr(std::string("settings.network.direct_connections.") +
            mesh::DirectAudienceName(parsed.value_or(mesh::DirectAudience::Contacts)));
}

/** Anchor ShowActions float menus under the right side of a settings choice row. */
ui::Vector2i ChoiceRowMenuPosition(ui::Event& ev) {
  ui::Element* target = ev.GetCurrentElement();
  if (!target) {
    target = ev.GetTargetElement();
  }
  if (!target) {
    return {0, 0};
  }

  ui::Element* anchor = target;
  const int child_count = target->GetNumChildren();
  for (int i = 0; i < child_count; ++i) {
    ui::Element* child = target->GetChild(i);
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

void SettingsController::BindUnlockEnsure(UnlockEnsurePorts ports) {
  unlock_ensure_ = std::move(ports);
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
  const bool advanced = ui_state_.show_advanced_settings == "on";
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    const SettingsSectionListItem item = handler->ListItem();
    if (item.advanced_only && !advanced) {
      continue;
    }
    sections_.push_back({.id = item.id.c_str(),
                         .title = item.title.c_str(),
                         .subtitle = item.subtitle.c_str(),
                         .attention = false});
  }
  ApplySectionAttention();
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
  ui_state_.amp_listen_multiaddr = bindings_.amp_listen_multiaddr.c_str();
  ui_state_.mesh_status_message = bindings_.mesh_status_message.c_str();
  ui_state_.reachability_status_label = bindings_.reachability_status_label.c_str();
  ui_state_.reachability_summary = bindings_.reachability_summary.c_str();
  ui_state_.reachability_help_kind = bindings_.reachability_help_kind.c_str();
  ui_state_.show_connection_card = bindings_.show_connection_card;
  ui_state_.show_reachability_help = bindings_.show_reachability_help;
  ui_state_.circuit_relay_enabled = bindings_.circuit_relay_enabled.c_str();
  ui_state_.show_circuit_relay_toggle = bindings_.show_circuit_relay_toggle;
  ui_state_.media_relay_enabled = bindings_.media_relay_enabled.c_str();
  ui_state_.show_media_relay_toggle = bindings_.show_media_relay_toggle;
  ui_state_.dht_enabled = bindings_.dht_enabled.c_str();
  ui_state_.show_dht_toggle = bindings_.show_dht_toggle;
  ui_state_.prefer_contacts_for_routing = bindings_.prefer_contacts_for_routing.c_str();
  ui_state_.direct_connections = bindings_.direct_connections.c_str();
  ui_state_.trusted_relays_only = bindings_.trusted_relays_only.c_str();
  ui_state_.reachable_without_relay = bindings_.reachable_without_relay.c_str();
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
  ui_state_.profile_icon_src = bindings_.profile_icon_src.c_str();
  ui_state_.profile_has_icon = bindings_.profile_has_icon;
  ui_state_.profile_icon_uploading = bindings_.profile_icon_uploading;
  ui_state_.profile_show_clear_icon = bindings_.profile_show_clear_icon;
  ui_state_.profile_avatar_letter = bindings_.profile_avatar_letter.c_str();
  ui_state_.profile_avatar_tone = bindings_.profile_avatar_tone;
  ui_state_.auto_renew_registration = bindings_.auto_renew_registration.c_str();
  ui_state_.show_notifications = bindings_.show_notifications.c_str();
  ui_state_.stay_connected = bindings_.stay_connected.c_str();
  ui_state_.brief_llm_key_masked = bindings_.brief_llm_key_masked.c_str();
  ui_state_.appearance = bindings_.appearance.c_str();
  ui_state_.appearance_label = bindings_.appearance_label.c_str();
  ui_state_.language = bindings_.language.c_str();
  ui_state_.language_label = bindings_.language_label.c_str();
  ui_state_.reduce_transparency = bindings_.reduce_transparency.c_str();
  ui_state_.call_diagnostics = bindings_.call_diagnostics.c_str();
  ui_state_.crash_reports_enabled = bindings_.crash_reports_enabled.c_str();
  ui_state_.show_advanced_settings = bindings_.show_advanced_settings.c_str();
  ui_state_.pin_protection_status = bindings_.pin_protection_status.c_str();
  ui_state_.pin_protection_is_default = bindings_.pin_protection_is_default;
  ui_state_.pin_protection_not_setup = bindings_.pin_protection_not_setup;
  ui_state_.security_can_change_pin = bindings_.security_can_change_pin;
  ui_state_.security_can_export_link = bindings_.security_can_export_link;
  ui_state_.security_has_recovery_phrase = bindings_.security_has_recovery_phrase;
  ui_state_.group_invite_policy = bindings_.group_invite_policy.c_str();
  ui_state_.group_invite_policy_label = bindings_.group_invite_policy_label.c_str();
  ui_state_.call_invite_policy = bindings_.call_invite_policy.c_str();
  ui_state_.call_invite_policy_label = bindings_.call_invite_policy_label.c_str();
  ui_state_.attachment_download_policy = bindings_.attachment_download_policy.c_str();
  ui_state_.attachment_download_policy_label = bindings_.attachment_download_policy_label.c_str();
  ui_state_.tool_permissions_summary = bindings_.tool_permissions_summary.c_str();
  ui_state_.tool_permissions_has_saved = bindings_.tool_permissions_has_saved;

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
  bindings_.amp_listen_multiaddr = ui_state_.amp_listen_multiaddr.c_str();
  bindings_.mesh_status_message = ui_state_.mesh_status_message.c_str();
  bindings_.reachability_status_label = ui_state_.reachability_status_label.c_str();
  bindings_.reachability_summary = ui_state_.reachability_summary.c_str();
  bindings_.reachability_help_kind = ui_state_.reachability_help_kind.c_str();
  bindings_.show_connection_card = ui_state_.show_connection_card;
  bindings_.show_reachability_help = ui_state_.show_reachability_help;
  bindings_.circuit_relay_enabled = ui_state_.circuit_relay_enabled.c_str();
  bindings_.show_circuit_relay_toggle = ui_state_.show_circuit_relay_toggle;
  bindings_.media_relay_enabled = ui_state_.media_relay_enabled.c_str();
  bindings_.show_media_relay_toggle = ui_state_.show_media_relay_toggle;
  bindings_.dht_enabled = ui_state_.dht_enabled.c_str();
  bindings_.show_dht_toggle = ui_state_.show_dht_toggle;
  bindings_.prefer_contacts_for_routing = ui_state_.prefer_contacts_for_routing.c_str();
  bindings_.direct_connections = ui_state_.direct_connections.c_str();
  bindings_.trusted_relays_only = ui_state_.trusted_relays_only.c_str();
  bindings_.reachable_without_relay = ui_state_.reachable_without_relay.c_str();
  bindings_.direct_connections_label = DirectConnectionsDisplayLabel(ui_state_.direct_connections).c_str();
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
  profile_actions_.ShowInvite(ui_state_.profile_contact_locator);
  bindings_.profile_show_rotate = ui_state_.profile_show_rotate;
  bindings_.profile_icon_src = ui_state_.profile_icon_src.c_str();
  bindings_.profile_has_icon = ui_state_.profile_has_icon;
  bindings_.profile_icon_uploading = ui_state_.profile_icon_uploading;
  bindings_.profile_show_clear_icon = ui_state_.profile_show_clear_icon;
  RefreshProfileAvatarGlyph(ui_state_);
  bindings_.profile_avatar_letter = ui_state_.profile_avatar_letter.c_str();
  bindings_.profile_avatar_tone = ui_state_.profile_avatar_tone;
  bindings_.auto_renew_registration = ui_state_.auto_renew_registration.c_str();
  bindings_.show_notifications = ui_state_.show_notifications.c_str();
  bindings_.stay_connected = ui_state_.stay_connected.c_str();
  bindings_.brief_llm_key_masked = ui_state_.brief_llm_key_masked.c_str();
  bindings_.appearance = ui_state_.appearance.c_str();
  bindings_.appearance_label = ui_state_.appearance_label.c_str();
  bindings_.language = ui_state_.language.c_str();
  bindings_.language_label = ui_state_.language_label.c_str();
  bindings_.reduce_transparency = ui_state_.reduce_transparency.c_str();
  bindings_.call_diagnostics = ui_state_.call_diagnostics.c_str();
  bindings_.crash_reports_enabled = ui_state_.crash_reports_enabled.c_str();
  bindings_.show_advanced_settings = ui_state_.show_advanced_settings.c_str();
  bindings_.advanced = ui_state_.show_advanced_settings == "on";
  bindings_.profile_label = ui_state_.profile_label.c_str();
  bindings_.config_dir = ui_state_.config_dir.c_str();
  bindings_.data_dir = ui_state_.data_dir.c_str();
  bindings_.profile_dir = ui_state_.profile_dir.c_str();
  bindings_.profile_size_label = ui_state_.profile_size_label.c_str();
  bindings_.attachment_cache_size_label = ui_state_.attachment_cache_size_label.c_str();
  bindings_.attachment_download_policy = ui_state_.attachment_download_policy.c_str();
  bindings_.attachment_download_policy_label = ui_state_.attachment_download_policy_label.c_str();
  bindings_.pin_protection_status = ui_state_.pin_protection_status.c_str();
  bindings_.pin_protection_is_default = ui_state_.pin_protection_is_default;
  bindings_.pin_protection_not_setup = ui_state_.pin_protection_not_setup;
  bindings_.security_can_change_pin = ui_state_.security_can_change_pin;
  bindings_.security_can_export_link = ui_state_.security_can_export_link;
  bindings_.security_has_recovery_phrase = ui_state_.security_has_recovery_phrase;
  bindings_.group_invite_policy = ui_state_.group_invite_policy.c_str();
  bindings_.group_invite_policy_label = ui_state_.group_invite_policy_label.c_str();
  bindings_.call_invite_policy = ui_state_.call_invite_policy.c_str();
  bindings_.call_invite_policy_label = ui_state_.call_invite_policy_label.c_str();
  bindings_.tool_permissions_summary = ui_state_.tool_permissions_summary.c_str();
  bindings_.tool_permissions_has_saved = ui_state_.tool_permissions_has_saved;
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
  if (selected_id_ == "storage") {
    cas_library_.Refresh();
  }
  if (selected_id_ == "security") {
    own_devices_.Refresh();
  }

  const BootstrapResult& bootstrap = Store().Snapshot();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    handler->SyncFromSession(bootstrap, ui_state_);
  }
  if (commands_.last_mesh_error) {
    ui_state_.mesh_status_message = commands_.last_mesh_error();
  } else {
    ui_state_.mesh_status_message.clear();
  }
  if (commands_.amp_listen_multiaddr) {
    ui_state_.amp_listen_multiaddr = commands_.amp_listen_multiaddr();
  } else {
    ui_state_.amp_listen_multiaddr.clear();
  }
  ApplyReachability();
  ApplySupportDiscovery();
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

bool SettingsController::RegisterModel(ui::Context* context) {
  if (!context) {
    return false;
  }
  context_ = context;

  return DataModelHost::Instance().Register(context, "settings", [this](ui::DataModelConstructor& ctor) {
    auto& controller = *this;
    if (auto section_handle = ctor.RegisterStruct<SectionListRow>()) {
      section_handle.RegisterMember("id", &SectionListRow::id);
      section_handle.RegisterMember("title", &SectionListRow::title);
      section_handle.RegisterMember("subtitle", &SectionListRow::subtitle);
      section_handle.RegisterMember("attention", &SectionListRow::attention);
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
    cas_library_.Bind(ctor);
    ctor.Bind("relay_base_url", &controller.bindings_.relay_base_url);
    ctor.Bind("directory_base_url", &controller.bindings_.directory_base_url);
    ctor.Bind("registration_base_url", &controller.bindings_.registration_base_url);
    ctor.Bind("node_enabled", &controller.bindings_.node_enabled);
    ctor.Bind("show_node_toggle", &controller.bindings_.show_node_toggle);
    ctor.Bind("amp_listen_multiaddr", &controller.bindings_.amp_listen_multiaddr);
    ctor.Bind("mesh_status_message", &controller.bindings_.mesh_status_message);
    ctor.Bind("reachability_status_label", &controller.bindings_.reachability_status_label);
    ctor.Bind("reachability_summary", &controller.bindings_.reachability_summary);
    ctor.Bind("reachability_help_kind", &controller.bindings_.reachability_help_kind);
    ctor.Bind("show_connection_card", &controller.bindings_.show_connection_card);
    ctor.Bind("show_reachability_help", &controller.bindings_.show_reachability_help);
    ctor.Bind("circuit_relay_enabled", &controller.bindings_.circuit_relay_enabled);
    ctor.Bind("show_circuit_relay_toggle", &controller.bindings_.show_circuit_relay_toggle);
    ctor.Bind("media_relay_enabled", &controller.bindings_.media_relay_enabled);
    ctor.Bind("show_media_relay_toggle", &controller.bindings_.show_media_relay_toggle);
    ctor.Bind("dht_enabled", &controller.bindings_.dht_enabled);
    ctor.Bind("show_dht_toggle", &controller.bindings_.show_dht_toggle);
    ctor.Bind("prefer_contacts_for_routing", &controller.bindings_.prefer_contacts_for_routing);
    ctor.Bind("direct_connections_label", &controller.bindings_.direct_connections_label);
    ctor.Bind("trusted_relays_only", &controller.bindings_.trusted_relays_only);
    ctor.Bind("reachable_without_relay", &controller.bindings_.reachable_without_relay);
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
    profile_actions_.Bind(ctor);
    ctor.Bind("profile_show_rotate", &controller.bindings_.profile_show_rotate);
    ctor.Bind("profile_icon_src", &controller.bindings_.profile_icon_src);
    ctor.Bind("profile_has_icon", &controller.bindings_.profile_has_icon);
    ctor.Bind("profile_icon_uploading", &controller.bindings_.profile_icon_uploading);
    ctor.Bind("profile_show_clear_icon", &controller.bindings_.profile_show_clear_icon);
    ctor.Bind("profile_avatar_letter", &controller.bindings_.profile_avatar_letter);
    ctor.Bind("profile_avatar_tone", &controller.bindings_.profile_avatar_tone);
    ctor.Bind("auto_renew_registration", &controller.bindings_.auto_renew_registration);
    ctor.Bind("show_notifications", &controller.bindings_.show_notifications);
    ctor.Bind("stay_connected", &controller.bindings_.stay_connected);
    controller.bindings_.stay_connected_available = Platform::Detect() == PlatformKind::Android;
    ctor.Bind("stay_connected_available", &controller.bindings_.stay_connected_available);
    ctor.Bind("brief_llm_key_masked", &controller.bindings_.brief_llm_key_masked);
    ctor.Bind("appearance", &controller.bindings_.appearance);
    ctor.Bind("appearance_label", &controller.bindings_.appearance_label);
    ctor.Bind("language", &controller.bindings_.language);
    ctor.Bind("language_label", &controller.bindings_.language_label);
    ctor.Bind("reduce_transparency", &controller.bindings_.reduce_transparency);
    ctor.Bind("call_diagnostics", &controller.bindings_.call_diagnostics);
    ctor.Bind("crash_reports_enabled", &controller.bindings_.crash_reports_enabled);
    ctor.Bind("show_advanced_settings", &controller.bindings_.show_advanced_settings);
    ctor.Bind("advanced", &controller.bindings_.advanced);
    ctor.Bind("profile_label", &controller.bindings_.profile_label);
    ctor.Bind("config_dir", &controller.bindings_.config_dir);
    ctor.Bind("data_dir", &controller.bindings_.data_dir);
    ctor.Bind("profile_dir", &controller.bindings_.profile_dir);
    ctor.Bind("profile_size_label", &controller.bindings_.profile_size_label);
    ctor.Bind("attachment_cache_size_label", &controller.bindings_.attachment_cache_size_label);
    ctor.Bind("attachment_download_policy", &controller.bindings_.attachment_download_policy);
    ctor.Bind("attachment_download_policy_label", &controller.bindings_.attachment_download_policy_label);
    ctor.Bind("pin_protection_status", &controller.bindings_.pin_protection_status);
    ctor.Bind("pin_protection_is_default", &controller.bindings_.pin_protection_is_default);
    ctor.Bind("pin_protection_not_setup", &controller.bindings_.pin_protection_not_setup);
    ctor.Bind("security_can_change_pin", &controller.bindings_.security_can_change_pin);
    ctor.Bind("security_can_export_link", &controller.bindings_.security_can_export_link);
    ctor.Bind("security_has_recovery_phrase", &controller.bindings_.security_has_recovery_phrase);
    ctor.Bind("group_invite_policy", &controller.bindings_.group_invite_policy);
    ctor.Bind("group_invite_policy_label", &controller.bindings_.group_invite_policy_label);
    ctor.Bind("call_invite_policy_label", &controller.bindings_.call_invite_policy_label);
    ctor.Bind("tool_permissions_summary", &controller.bindings_.tool_permissions_summary);
    ctor.Bind("tool_permissions_has_saved", &controller.bindings_.tool_permissions_has_saved);
    ctor.Bind("app_name", &controller.bindings_.app_name);
    ctor.Bind("app_version", &controller.bindings_.app_version);
    ctor.Bind("support_visible", &controller.bindings_.support_visible);
    ctor.Bind("support_display_name", &controller.bindings_.support_display_name);
    ctor.Bind("support_subtitle", &controller.bindings_.support_subtitle);
    security_actions_.Bind(ctor);
    own_devices_.Bind(ctor);
    ctor.Bind("status", &controller.status_);
    ctor.BindEventCallback("select_section", DataEvent<std::string>([this](const std::string& id) { OnSelectSection(id); }));
    ctor.BindEventCallback("back_to_list", DataEvent<>([this]() { OnBackToList(); }));
    ctor.BindEventCallback("reset_section", DataEvent<std::string>([this](const std::string& id) { OnResetSection(id); }));
    ctor.BindEventCallback("open_support_chat", DataEvent<>([this]() { OnOpenSupportChat(); }));
    ctor.BindEventCallback("on_llm_field_changed", DataEvent<>([this]() { MarkSectionDirty("llm"); }));
    ctor.BindEventCallback("on_llm_preset_changed", DataEventAt([this](ui::Event& ev) { OnLlmPresetChanged(ev); }));
    ctor.BindEventCallback("on_choose_theme", DataEventAt([this](ui::Event& ev) { OnChooseTheme(ev); }));
    ctor.BindEventCallback("on_choose_language", DataEventAt([this](ui::Event& ev) { OnChooseLanguage(ev); }));
    ctor.BindEventCallback("on_choose_group_invite_policy", DataEventAt([this](ui::Event& ev) { OnChooseGroupInvitePolicy(ev); }));
    ctor.BindEventCallback("on_choose_call_invite_policy", DataEventAt([this](ui::Event& ev) { OnChooseCallInvitePolicy(ev); }));
    ctor.BindEventCallback("on_choose_direct_connections", DataEventAt([this](ui::Event& ev) { OnChooseDirectConnections(ev); }));
    ctor.BindEventCallback("on_choose_attachment_download_policy", DataEventAt([this](ui::Event& ev) { OnChooseAttachmentDownloadPolicy(ev); }));
    ctor.BindEventCallback("drain_pending_attachment_media", DataEvent<>([this]() { OnDrainPendingAttachmentMedia(); }));
    ctor.BindEventCallback("clear_downloaded_attachments", DataEvent<>([this]() { OnClearDownloadedAttachments(); }));
    ctor.BindEventCallback("toggle_show_notifications", DataEvent<>([this]() { ToggleShowNotifications(); }));
    ctor.BindEventCallback("toggle_stay_connected", DataEvent<>([this]() { ToggleStayConnected(); }));
    ctor.BindEventCallback("toggle_reduce_transparency", DataEvent<>([this]() { ToggleReduceTransparency(); }));
    ctor.BindEventCallback("toggle_call_diagnostics", DataEvent<>([this]() { ToggleCallDiagnostics(); }));
    ctor.BindEventCallback("toggle_show_advanced_settings", DataEvent<>([this]() { ToggleShowAdvancedSettings(); }));
    ctor.BindEventCallback("toggle_crash_reports", DataEvent<>([this]() { ToggleCrashReports(); }));
    ctor.BindEventCallback("toggle_auto_renew_registration", DataEvent<>([this]() { ToggleAutoRenewRegistration(); }));
    ctor.BindEventCallback("on_integrations_field_changed", DataEvent<>([this]() { MarkSectionDirty("integrations"); }));
    ctor.BindEventCallback("on_network_field_changed", DataEvent<>([this]() { MarkSectionDirty("network"); }));
    ctor.BindEventCallback("toggle_node_enabled", DataEvent<>([this]() { ToggleNodeEnabled(); }));
    ctor.BindEventCallback("retest_reachability", DataEvent<>([this]() { RetestReachability(); }));
    ctor.BindEventCallback("try_upnp_port", DataEvent<>([this]() { TryUpnpPort(); }));
    ctor.BindEventCallback("show_reachability_help", DataEvent<>([this]() { ShowReachabilityHelp(); }));
    ctor.BindEventCallback("dismiss_reachability_help", DataEvent<>([this]() { DismissReachabilityHelp(); }));
    ctor.BindEventCallback("toggle_circuit_relay", DataEvent<>([this]() { ToggleCircuitRelay(); }));
    ctor.BindEventCallback("toggle_media_relay", DataEvent<>([this]() { ToggleMediaRelay(); }));
    ctor.BindEventCallback("toggle_dht", DataEvent<>([this]() { ToggleDht(); }));
    ctor.BindEventCallback("toggle_prefer_contacts", DataEvent<>([this]() { TogglePreferContacts(); }));
    ctor.BindEventCallback("toggle_trusted_relays", DataEvent<>([this]() { ToggleTrustedRelays(); }));
    ctor.BindEventCallback("toggle_reachable_without_relay", DataEvent<>([this]() { ToggleReachableWithoutRelay(); }));
    // Persist on blur only when the field differs from last loaded/saved nickname.
    ctor.BindEventCallback("on_profile_nickname_commit", DataEvent<>([this]() { CommitProfileNickname(/*show_toast=*/false); }));
    ctor.BindEventCallback("add_mcp_server", DataEvent<>([this]() { OnAddMcpServer(); }));
    ctor.BindEventCallback("remove_mcp_server", DataEvent<int>([this](int index) { OnRemoveMcpServer(index); }));
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
  host.Dirty("settings", "amp_listen_multiaddr");
  host.Dirty("settings", "mesh_status_message");
  host.Dirty("settings", "reachability_status_label");
  host.Dirty("settings", "reachability_summary");
  host.Dirty("settings", "reachability_help_kind");
  host.Dirty("settings", "show_connection_card");
  host.Dirty("settings", "show_reachability_help");
  host.Dirty("settings", "circuit_relay_enabled");
  host.Dirty("settings", "show_circuit_relay_toggle");
  host.Dirty("settings", "media_relay_enabled");
  host.Dirty("settings", "show_media_relay_toggle");
  host.Dirty("settings", "dht_enabled");
  host.Dirty("settings", "show_dht_toggle");
  host.Dirty("settings", "prefer_contacts_for_routing");
  host.Dirty("settings", "direct_connections_label");
  host.Dirty("settings", "trusted_relays_only");
  host.Dirty("settings", "reachable_without_relay");
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
  host.Dirty("settings", "profile_icon_src");
  host.Dirty("settings", "profile_has_icon");
  host.Dirty("settings", "profile_icon_uploading");
  host.Dirty("settings", "profile_show_clear_icon");
  host.Dirty("settings", "profile_avatar_letter");
  host.Dirty("settings", "profile_avatar_tone");
  host.Dirty("settings", "auto_renew_registration");
  host.Dirty("settings", "show_notifications");
  host.Dirty("settings", "stay_connected");
  host.Dirty("settings", "brief_llm_key_masked");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "appearance_label");
  host.Dirty("settings", "language");
  host.Dirty("settings", "language_label");
  host.Dirty("settings", "reduce_transparency");
  host.Dirty("settings", "call_diagnostics");
  host.Dirty("settings", "crash_reports_enabled");
  host.Dirty("settings", "show_advanced_settings");
  host.Dirty("settings", "advanced");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "profile_size_label");
  host.Dirty("settings", "attachment_cache_size_label");
  host.Dirty("settings", "attachment_download_policy");
  host.Dirty("settings", "attachment_download_policy_label");
  host.Dirty("settings", "pin_protection_status");
  host.Dirty("settings", "pin_protection_is_default");
  host.Dirty("settings", "pin_protection_not_setup");
  host.Dirty("settings", "security_can_change_pin");
  host.Dirty("settings", "security_can_export_link");
  host.Dirty("settings", "security_has_recovery_phrase");
  host.Dirty("settings", "recovery_phrase");
  host.Dirty("settings", "recovery_phrase_pin");
  host.Dirty("settings", "group_invite_policy");
  host.Dirty("settings", "group_invite_policy_label");
  host.Dirty("settings", "call_invite_policy_label");
  host.Dirty("settings", "tool_permissions_summary");
  host.Dirty("settings", "tool_permissions_has_saved");
  host.Dirty("settings", "app_name");
  host.Dirty("settings", "app_version");
  host.Dirty("settings", "support_visible");
  host.Dirty("settings", "support_display_name");
  host.Dirty("settings", "support_subtitle");
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
  MountSelectedSettingsSection();
  // Select widgets can emit a spurious change on the frame after remount.
  AppRuntime::PostUI([this]() {
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
    MountSelectedSettingsSection();
    suppress_auto_save_ = false;
    UiEditSession::Instance().EndRemount();
  });
}

void SettingsController::MountSelectedSettingsSection() {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return;
  }
  ui::ElementDocument* doc = context_->GetDocument(0);
  if (!doc) {
    return;
  }
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  const bool use_sheet_mount = show_detail_ || in_account_sheet_ || chrome.account_sheet_open;
  const char* mount_id = use_sheet_mount ? "settings-section-mount-sheet" : "settings-section-mount-pane";
  ui::Element* mount = doc->GetElementById(mount_id);
  if (!mount) {
    // Fallback: try the other mount if layout migration left only one present.
    mount = doc->GetElementById(use_sheet_mount ? "settings-section-mount-pane" : "settings-section-mount-sheet");
  }
  if (!mount) {
    return;
  }
  if (selected_id_.empty()) {
    RmlMount::MountInner(mount, {});
    return;
  }
  const std::string key = "settings_section_" + std::string(selected_id_.c_str());
  const std::string body = ViewCatalog::LoadBody(key);
  if (body.empty()) {
    log().error << "Failed to load settings section body for '" << key << "'";
    return;
  }
  RmlMount::MountInner(mount, body);
  DirtyAll();
}

void SettingsController::OnShellLayoutSynced() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (!suppress_auto_save_) {
    // A remount this controller did not start (e.g. the list pane was collapsed) still empties the
    // detail pane's section mount; fill it again or the open section shows blank.
    if (chrome.settings_detail_primary && !selected_id_.empty()) {
      suppress_auto_save_ = true;
      UiEditSession::Instance().BeginRemount();
      FinishPaneResync();
    }
    return;
  }
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

  const ui::String saved_id = selected_id_;
  const ui::String saved_title = selected_title_;
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

void SettingsController::MarkSectionDirty(const std::string& section_id, const bool show_toast) {
  if (suppress_auto_save_ || UiEditSession::Instance().RemountBlocking()) {
    log().info << "MarkSectionDirty(" << section_id << ") suppressed during UI transition";
    return;
  }

  SettingsSectionHandler* handler = FindHandler(section_id);
  if (!handler || !handler->IsWritable()) {
    return;
  }

  dirty_sections_.insert(section_id);
  if (show_toast) {
    quiet_sections_.erase(section_id);
  } else {
    quiet_sections_.insert(section_id);
  }
  if (handler->FlushMode() == SettingsFlushMode::Debounced) {
    debounce_deadline_ms_ = SDL_GetTicks() + kDebounceMs;
    return;
  }

  FlushSection(section_id, show_toast);
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
  if (quiet_sections_.erase(section_id) > 0) {
    show_toast = false;
  }
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

  // Field commits must not DirtyAll/DirtyNavChrome: SetValue/remount re-enters blur and
  // looks like per-keystroke focus loss. Toast paths refresh via DirtyFeedback in show_toast.
  if (section_id == "profile" && !show_toast) {
    return true;
  }

  DirtyAll();
  if (show_toast) {
    MaybeShowSaveToast(section_id);
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
      },
      {});
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
    AppRuntime::PostUI([this]() {
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
  AppRuntime::PostUI([this]() {
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
  AppRuntime::PostUI([this]() {
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

void SettingsController::OnLlmPresetChanged(ui::Event& ev) {
  if (suppress_auto_save_) {
    return;
  }

  const ui::String value = EventValue(ev);
  if (!value.empty()) {
    bindings_.llm_preset = value;
  }

  PullBindingsToUiState();
  // Translate preset intent → precise default model before flush/normalize.
  const std::string preset = ui_state_.llm_preset;
  if (const std::string default_model = DefaultModelForPreset(preset); !default_model.empty()) {
    ui_state_.llm_model = default_model;
    bindings_.llm_model = default_model.c_str();
  }
  if (preset != "custom") {
    // Known presets own base_url; clear custom URL so Normalize/ApplyPreset fill it.
    AppConfig scratch;
    ApplyPreset(scratch, preset, {});
    ui_state_.llm_base_url = scratch.llm.base_url;
    bindings_.llm_base_url = scratch.llm.base_url.c_str();
  }
  DataModelHost::Instance().Dirty("settings", "llm_model");
  DataModelHost::Instance().Dirty("settings", "llm_base_url");
  DataModelHost::Instance().Dirty("settings", "llm_preset");

  const SettingsSectionHandler* handler = FindHandler("llm");
  if (!handler) {
    return;
  }
  if (handler->IsPersisted(ui_state_, Store().Snapshot())) {
    return;
  }

  FlushSection("llm");
}

void SettingsController::OnChooseTheme(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);

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

void SettingsController::OnChooseLanguage(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);

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

void SettingsController::OnChooseDirectConnections(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);
  const std::string current =
      bindings_.direct_connections.empty() ? "contacts" : std::string(bindings_.direct_connections.c_str());
  std::vector<ContextMenuAction> actions;
  for (const mesh::DirectAudience audience :
       {mesh::DirectAudience::Everyone, mesh::DirectAudience::Contacts, mesh::DirectAudience::Friendly, mesh::DirectAudience::Nobody}) {
    const std::string id = mesh::DirectAudienceName(audience);
    actions.push_back({.id = id,
                       .label = DirectConnectionsDisplayLabel(id),
                       .enabled = {},
                       .run = [this, id]() { ApplyDirectConnectionsChoice(id); },
                       .icon = {},
                       .danger = false,
                       .selected = current == id});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyDirectConnectionsChoice(const std::string& audience) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.direct_connections = audience.c_str();
  bindings_.direct_connections_label = DirectConnectionsDisplayLabel(audience).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("network");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnChooseCallInvitePolicy(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);
  const std::string current =
      bindings_.call_invite_policy.empty() ? "everyone" : std::string(bindings_.call_invite_policy.c_str());
  std::vector<ContextMenuAction> actions;
  for (const char* id : {"everyone", "contacts_only", "nobody"}) {
    const std::string policy_id = id;
    actions.push_back({.id = policy_id,
                       .label = CallInvitePolicyDisplayLabel(policy_id),
                       .enabled = {},
                       .run = [this, policy_id]() { ApplyCallInvitePolicyChoice(policy_id); },
                       .icon = {},
                       .danger = false,
                       .selected = current == policy_id});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyCallInvitePolicyChoice(const std::string& policy) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.call_invite_policy = policy.c_str();
  bindings_.call_invite_policy_label = CallInvitePolicyDisplayLabel(policy).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("security");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnChooseGroupInvitePolicy(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);

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

void SettingsController::OnChooseAttachmentDownloadPolicy(ui::Event& ev) {
  const ui::Vector2i position = ChoiceRowMenuPosition(ev);
  const std::string current = bindings_.attachment_download_policy.empty()
                                  ? "smart"
                                  : std::string(bindings_.attachment_download_policy.c_str());

  static const char* kPolicyIds[] = {"smart", "always_auto", "on_demand"};
  std::vector<ContextMenuAction> actions;
  actions.reserve(3);
  for (const char* id : kPolicyIds) {
    const std::string policy_id = id;
    actions.push_back({.id = policy_id,
                       .label = AttachmentDownloadPolicyDisplayLabel(policy_id),
                       .enabled = {},
                       .run =
                           [this, policy_id]() {
                             ApplyAttachmentDownloadPolicyChoice(policy_id);
                           },
                       .icon = {},
                       .danger = false,
                       .selected = current == policy_id});
  }
  ContextMenuHost::Instance().ShowActions(position, std::move(actions));
}

void SettingsController::ApplyAttachmentDownloadPolicyChoice(const std::string& policy) {
  if (suppress_auto_save_) {
    return;
  }
  bindings_.attachment_download_policy = policy.c_str();
  bindings_.attachment_download_policy_label = AttachmentDownloadPolicyDisplayLabel(policy).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("storage");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnDrainPendingAttachmentMedia() {
  if (!commands_.drain_pending_attachment_media) {
    ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Attachment downloads are not available"));
    return;
  }
  commands_.drain_pending_attachment_media();
  UserFeedback::Ok(Tr("settings.storage.pending_media_started"));
}

void SettingsController::OnClearDownloadedAttachments() {
  if (!shell_feedback_.show_confirm) {
    return;
  }
  shell_feedback_.show_confirm(Tr("settings.storage.clear_attachments_confirm_title"),
                               Tr("settings.storage.clear_attachments_confirm_message"),
                               [this](const bool ok) {
                                 if (!ok) {
                                   return;
                                 }
                                 PerformClearDownloadedAttachments();
                               },
                               {});
}

void SettingsController::PerformClearDownloadedAttachments() {
  if (!commands_.clear_downloaded_attachments) {
    ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Attachment storage is not available"));
    return;
  }
  if (auto cleared = commands_.clear_downloaded_attachments(); !cleared) {
    ReportFailure(cleared.error());
    return;
  }
  SyncBindingsFromSession();
  DirtyAll();
  UserFeedback::Ok(Tr("settings.storage.clear_attachments_done"));
}

void SettingsController::RefreshLocalizedChrome() {
  SyncBindingsFromSession();
  InitSections(); // after the sync: the advanced flag comes from the profile prefs
  ApplySupportDiscovery();
  if (!selected_id_.empty()) {
    if (const SettingsSectionHandler* handler = FindHandler(selected_id_.c_str())) {
      selected_title_ = handler->ListItem().title.c_str();
    }
  }
  DirtyAll();
}

void SettingsController::SyncSupportDiscovery() {
  ApplySupportDiscovery();
  DataModelHost::Instance().Dirty("settings", "support_visible");
  DataModelHost::Instance().Dirty("settings", "support_display_name");
  DataModelHost::Instance().Dirty("settings", "support_subtitle");
}

void SettingsController::ApplySupportDiscovery() {
  bindings_.support_visible = false;
  bindings_.support_display_name.clear();
  bindings_.support_subtitle = Tr("support.entry.subtitle").c_str();
  if (!commands_.load_support_discovery) {
    return;
  }
  const std::optional<ClientCompatSupport> discovery = commands_.load_support_discovery();
  if (!discovery || !discovery->enabled || discovery->account_id.empty()) {
    return;
  }
  bindings_.support_visible = true;
  bindings_.support_display_name =
      discovery->display_name.empty() ? Tr("support.entry.title").c_str() : discovery->display_name.c_str();
}

void SettingsController::OnOpenSupportChat() {
  if (!commands_.open_support_chat) {
    ReportFailure(Error("Support is not available").WithUser(Tr("settings.support.unavailable")));
    return;
  }
  if (auto opened = commands_.open_support_chat(); !opened) {
    ReportFailure(opened.error());
  }
}

void SettingsController::OpenNetworkSettings() {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (chrome.nav_tab != NavTab::Me) {
    if (shell_navigation_.select_nav_tab) {
      shell_navigation_.select_nav_tab(NavTab::Me);
    }
    // OnNavTabActivated clears selection; open Network on the next UI turn.
    AppRuntime::PostUI([this]() { OnSelectSection("network"); });
    return;
  }
  OnSelectSection("network");
}

void SettingsController::ApplyReachability() {
  const bool mesh_ready = commands_.mesh_ready ? commands_.mesh_ready()
                                               : (commands_.messaging_ready && commands_.messaging_ready());
  ui_state_.show_connection_card =
      ui_state_.show_node_toggle && ui_state_.node_enabled == "on" && mesh_ready;
  ui_state_.show_circuit_relay_toggle = ui_state_.show_connection_card;
  ui_state_.show_media_relay_toggle = ui_state_.show_connection_card;
  ui_state_.show_dht_toggle = ui_state_.show_connection_card;
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
    const auto& cfg = Store().Snapshot().config.mesh;
    ui_state_.circuit_relay_enabled = cfg.capabilities.circuit_relay ? "on" : "off";
    ui_state_.media_relay_enabled = cfg.capabilities.media_relay ? "on" : "off";
    ui_state_.dht_enabled = cfg.capabilities.dht ? "on" : "off";
    ui_state_.prefer_contacts_for_routing = cfg.prefer_contacts_for_routing ? "on" : "off";
    ui_state_.direct_connections = mesh::DirectAudienceName(cfg.direct_connections);
    ui_state_.trusted_relays_only = cfg.trusted_relays_only ? "on" : "off";
    ui_state_.reachable_without_relay = cfg.reachable_without_relay ? "on" : "off";
  }
  PushUiStateToBindings();
  ApplySectionAttention();
}

bool SettingsController::ComputeNetworkAttention() const {
  if (!ui_state_.show_node_toggle || ui_state_.node_enabled != "on") {
    return false;
  }
  if (!commands_.session_store || !commands_.load_reachability) {
    return false;
  }
  if (!commands_.mesh_ready) {
    if (!commands_.messaging_ready || !commands_.messaging_ready()) {
      return false;
    }
  } else if (!commands_.mesh_ready()) {
    return false;
  }
  const SettingsReachabilityView view = commands_.load_reachability();
  const std::string status_key = ReachabilityNudgeStatusKey(view.status);
  const std::string& acked =
      commands_.session_store().Snapshot().profile_prefs.reachability_nudge_acked_status;
  return ReachabilityNudgeActive(true, status_key, acked);
}

void SettingsController::ApplySectionAttention() {
  const bool network_attention = ComputeNetworkAttention();
  for (SectionListRow& section : sections_) {
    section.attention = section.id == "network" && network_attention;
  }
}

void SettingsController::SyncReachability() {
  ApplyReachability();
  DirtyAll();
}

void SettingsController::AckReachabilityNudge(const std::string& status_key) {
  if (status_key != kReachabilityNudgeOutboundOnly && status_key != kReachabilityNudgeBlocked) {
    return;
  }
  if (!commands_.session_store) {
    return;
  }
  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  const int new_sev = ReachabilityNudgeSeverity(status_key);
  if (new_sev <= ReachabilityNudgeSeverity(prefs.reachability_nudge_acked_status)) {
    return;
  }
  prefs.reachability_nudge_acked_status = status_key;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
    ReportFailure(saved.error());
    return;
  }
  ApplySectionAttention();
  if (commands_.refresh_nav_badges) {
    commands_.refresh_nav_badges();
  }
}

void SettingsController::ToggleNodeEnabled() {
  if (!bindings_.show_node_toggle) {
    return;
  }
  bindings_.node_enabled = bindings_.node_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  ApplyReachability();
  if (commands_.refresh_nav_badges) {
    commands_.refresh_nav_badges();
  }
  DirtyAll();
}

void SettingsController::RetestReachability() {
  if (Commands().run_reachability_probe) {
    Commands().run_reachability_probe(false);
  }
  SyncReachability();
}

void SettingsController::TryUpnpPort() {
  if (Commands().try_upnp_port_mapping) {
    Commands().try_upnp_port_mapping();
  }
  SyncReachability();
}

void SettingsController::ShowReachabilityHelp() {
  bindings_.show_reachability_help = true;
  PullBindingsToUiState();
  DirtyAll();
}

void SettingsController::DismissReachabilityHelp() {
  const std::string help_kind = bindings_.reachability_help_kind.c_str();
  bindings_.show_reachability_help = false;
  PullBindingsToUiState();
  if (help_kind == kReachabilityNudgeOutboundOnly || help_kind == kReachabilityNudgeBlocked) {
    AckReachabilityNudge(help_kind);
  }
  DirtyAll();
}

void SettingsController::ToggleCircuitRelay() {
  if (!bindings_.show_circuit_relay_toggle) {
    return;
  }
  bindings_.circuit_relay_enabled =
      bindings_.circuit_relay_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleMediaRelay() {
  if (!bindings_.show_media_relay_toggle) {
    return;
  }
  bindings_.media_relay_enabled =
      bindings_.media_relay_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleDht() {
  if (!bindings_.show_dht_toggle) {
    return;
  }
  bindings_.dht_enabled = bindings_.dht_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::TogglePreferContacts() {
  if (!bindings_.show_prefer_contacts_toggle) {
    return;
  }
  bindings_.prefer_contacts_for_routing =
      bindings_.prefer_contacts_for_routing == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleReachableWithoutRelay() {
  bindings_.reachable_without_relay =
      bindings_.reachable_without_relay == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleTrustedRelays() {
  bindings_.trusted_relays_only = bindings_.trusted_relays_only == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleShowNotifications() {
  bindings_.show_notifications =
      bindings_.show_notifications == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleStayConnected() {
  bindings_.stay_connected = bindings_.stay_connected == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleReduceTransparency() {
  bindings_.reduce_transparency =
      bindings_.reduce_transparency == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("appearance", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleCallDiagnostics() {
  bindings_.call_diagnostics =
      bindings_.call_diagnostics == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("appearance", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleShowAdvancedSettings() {
  bindings_.show_advanced_settings =
      bindings_.show_advanced_settings == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("about", /*show_toast=*/false);
  InitSections(); // the advanced-only sections appear or go
  PushUiStateToBindings();
  DirtyAll();
}

void SettingsController::ToggleCrashReports() {
  bindings_.crash_reports_enabled =
      bindings_.crash_reports_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("security", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleAutoRenewRegistration() {
  bindings_.auto_renew_registration =
      bindings_.auto_renew_registration == "auto" ? "off" : "auto";
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
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

void SettingsController::ShowStatus(const std::string& status) {
  status_ = status.c_str();
  DataModelHost::Instance().Dirty("settings", "status");
}

void SettingsController::Resync(const bool include_profile_nickname) {
  SyncBindingsFromSession();
  DirtyAll(include_profile_nickname);
}

SettingsUiState& SettingsController::UiState() {
  return ui_state_;
}

void SettingsController::PullFromView() {
  PullBindingsToUiState();
}

void SettingsController::PushToView(const bool include_profile_nickname) {
  PushUiStateToBindings();
  DirtyAll(include_profile_nickname);
}

void SettingsController::SaveProfileNickname() {
  CommitProfileNickname(/*show_toast=*/false);
}

void SettingsController::RequestSyncLayout(const char* reason) {
  if (shell_navigation_.request_sync_layout) {
    shell_navigation_.request_sync_layout(/*restore_focus_after=*/false, reason);
  }
}

Roe<void> SettingsController::ForgetToolPermissions() {
  auto* security = dynamic_cast<SecuritySettingsSection*>(FindHandler("security"));
  if (!security) {
    return Error("Security settings unavailable");
  }
  PullBindingsToUiState();
  if (auto reset = security->ResetToolPermissions(ui_state_, Store()); !reset) {
    return reset.error();
  }
  PushUiStateToBindings();
  DirtyAll(false);
  MaybeShowSaveToast("security");
  return {};
}

} // namespace pbr
