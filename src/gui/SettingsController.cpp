#include <stdexcept>
#include "gui/SettingsController.h"

#include "lib/ui/DataEvent.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/product/DevTools.h"
#include "foundation/platform/NativeFileDialog.h"

#include "foundation/data/AppPaths.h"
#include "foundation/data/BackendProfile.h"
#include "foundation/data/LlmPreset.h"
#include "common/SchemaVersion.h"
#include "foundation/data/SessionStore.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/net/ClientCompat.h"
#include "common/runtime/AppRuntime.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/ViewCatalog.h"
#include "feature/settings/AppearanceSettingsSection.h"
#include "feature/settings/BackendChoices.h"
#include "feature/settings/ReachabilityNudge.h"
#include "feature/settings/StorageSettingsSection.h"
#include "feature/settings/SettingsPages.h"
#include "feature/settings/SettingsPortsViews.h"
#include "gui/shell/DataModelHost.h"
#include "gui/shell/MascotLook.h"
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
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "gui/AccountInvite.h"

#include <ui/dom/Context.h>
#include <ui/Core.h>
#include <ui/data/DataModelHandle.h>
#include <ui/dom/Element.h>
#include <ui/dom/ElementDocument.h>
#include <ui/dom/Event.h>
#include <ui/base/SystemInterface.h>

#include <chrono>
#include <thread>
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

std::string BackendDisplayLabel(const std::string& env) {
  return Tr("settings.network.backend." + std::string(BackendEnvName(ParseBackendEnv(env))));
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
  foreground_listener_ = AppLifecycle::AddForegroundListener([this]() {
    RefreshBannerTip();
    RefreshNotificationTips();
  });
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

SettingsController::~SettingsController() {
  *alive_ = false;
  AppLifecycle::RemoveListener(foreground_listener_);
}

void SettingsController::RefreshBannerTip() {
  QueryBannerStyle([this, alive = alive_](BannerStyle style) {
    if (!*alive) {
      return;
    }
    const bool show = ShowCallBannerTip(style);
    if (show == bindings_.call_banner_tip_available) {
      return;
    }
    bindings_.call_banner_tip_available = show;
    DataModelHost::Instance().Dirty("settings", "call_banner_tip_available");
  });
}

void SettingsController::RefreshNotificationTips() {
  QueryNotificationHealth([this, alive = alive_](NotificationHealth health) {
    if (!*alive) {
      return;
    }
    last_health_ = health;
    ApplyNotificationTips();
  });
}

void SettingsController::ApplyNotificationTips() {
  const NotificationTips tips = ComputeNotificationTips(last_health_, bindings_.stay_connected == "on");
  auto set = [](bool& slot, bool value, const char* name) {
    if (slot != value) {
      slot = value;
      DataModelHost::Instance().Dirty("settings", name);
    }
  };
  set(bindings_.tip_notifications_blocked, tips.notifications_blocked, "tip_notifications_blocked");
  set(bindings_.tip_silenced, tips.silenced, "tip_silenced");
  set(bindings_.tip_battery, tips.battery, "tip_battery");
  set(bindings_.tip_autostart, tips.autostart, "tip_autostart");
  set(bindings_.stay_help_plain, tips.stay_help == PushGap::None, "stay_help_plain");
  set(bindings_.stay_help_build, tips.stay_help == PushGap::BuildHasNone, "stay_help_build");
  set(bindings_.stay_help_no_google, tips.stay_help == PushGap::NoGoogleServices, "stay_help_no_google");
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

void SettingsController::BindOpenHomeEdit(std::function<void()> open) {
  open_home_edit_ = std::move(open);
}

void SettingsController::ReconnectAccount(std::function<void(bool ok)> done) {
  profile_actions_.Register(/*quiet_success=*/true, std::move(done));
}

void SettingsController::BindRunWhenRegistrationIdle(std::function<void(std::function<void()>)> run) {
  profile_actions_.BindRunWhenRegistrationIdle(std::move(run));
}

void SettingsController::BindShowMyQr(std::function<void()> show) {
  profile_actions_.BindShowMyQr(std::move(show));
}

void SettingsController::BindOpenUiPerfTest(std::function<void()> open) {
  open_ui_perf_test_ = std::move(open);
}

void SettingsController::BindSimulateChatsLink(std::function<void(const std::string& state)> simulate) {
  simulate_chats_link_ = std::move(simulate);
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

void SettingsController::BindPinPrompt(PinPromptPorts ports) {
  pin_prompt_ = std::move(ports);
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
  // Advanced is listed (and so reachable) only while "show advanced settings" is on (Me → Help); Test options only
  // while the tester switch is on (five taps on the version).
  for (const SettingsPage* page :
       ListedSettingsPages(ui_state_.show_advanced_settings == "on", ui_state_.show_tester_options == "on")) {
    sections_.push_back({.id = page->id,
                         .title = Tr(page->title_key).c_str(),
                         .subtitle = "",
                         .group = page->home_group,
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
  ui_state_.backend_env = bindings_.backend_env.c_str();
  ui_state_.direct_connections = bindings_.direct_connections.c_str();
  ui_state_.trusted_relays_only = bindings_.trusted_relays_only.c_str();
  ui_state_.reachable_without_relay = bindings_.reachable_without_relay.c_str();
  ui_state_.show_prefer_contacts_toggle = bindings_.show_prefer_contacts_toggle;
  ui_state_.profile_nickname = bindings_.profile_nickname.c_str();
  ui_state_.profile_peer_id = bindings_.profile_peer_id.c_str();
  ui_state_.profile_relay_id = bindings_.profile_relay_id.c_str();
  ui_state_.profile_registered = bindings_.profile_registered.c_str();
  ui_state_.profile_registration_status = bindings_.profile_registration_status.c_str();
  ui_state_.profile_registration_expires = bindings_.profile_registration_expires.c_str();
  ui_state_.profile_register_label = bindings_.profile_register_label.c_str();
  ui_state_.profile_show_register = bindings_.profile_show_register;
  ui_state_.profile_register_primary = bindings_.profile_register_primary;
  ui_state_.profile_show_rotate = bindings_.profile_show_rotate;
  ui_state_.profile_icon_src = bindings_.profile_icon_src.c_str();
  ui_state_.profile_has_icon = bindings_.profile_has_icon;
  ui_state_.profile_icon_uploading = bindings_.profile_icon_uploading;
  ui_state_.profile_show_clear_icon = bindings_.profile_show_clear_icon;
  ui_state_.profile_avatar_letter = bindings_.profile_avatar_letter.c_str();
  ui_state_.profile_avatar_tone = bindings_.profile_avatar_tone;
  ui_state_.auto_renew_registration = bindings_.auto_renew_registration.c_str();
  ui_state_.show_notifications = bindings_.show_notifications.c_str();
  ui_state_.show_notification_preview = bindings_.show_notification_preview.c_str();
  ui_state_.stay_connected = bindings_.stay_connected.c_str();
  ui_state_.brief_llm_key_masked = bindings_.brief_llm_key_masked.c_str();
  ui_state_.appearance = bindings_.appearance.c_str();
  ui_state_.appearance_label = bindings_.appearance_label.c_str();
  ui_state_.language = bindings_.language.c_str();
  ui_state_.language_label = bindings_.language_label.c_str();
  ui_state_.reduce_transparency = bindings_.reduce_transparency.c_str();
  ui_state_.call_diagnostics = bindings_.call_diagnostics.c_str();
  ui_state_.home_show_dynamic = bindings_.home_show_dynamic.c_str();
  ui_state_.home_animated_logo = bindings_.home_animated_logo.c_str();
  ui_state_.crash_reports_enabled = bindings_.crash_reports_enabled.c_str();
  ui_state_.show_advanced_settings = bindings_.show_advanced_settings.c_str();
  ui_state_.show_tester_options = bindings_.show_tester_options.c_str();
  ui_state_.pin_protection_status = bindings_.pin_protection_status.c_str();
  ui_state_.pin_protection_is_default = bindings_.pin_protection_is_default;
  ui_state_.pin_protection_not_setup = bindings_.pin_protection_not_setup;
  ui_state_.security_can_change_pin = bindings_.security_can_change_pin;
  ui_state_.security_can_export_link = bindings_.security_can_export_link;
  ui_state_.security_has_recovery_phrase = bindings_.security_has_recovery_phrase;
  ui_state_.unlock_grace_minutes = bindings_.unlock_grace_minutes;
  ui_state_.unlock_grace_label = bindings_.unlock_grace_label.c_str();
  ui_state_.security_grace_shown = bindings_.security_grace_shown;
  ui_state_.security_biometric_shown = bindings_.security_biometric_shown;
  ui_state_.security_biometric_on = bindings_.security_biometric_on;
  ui_state_.security_biometric_label = bindings_.security_biometric_label.c_str();
  ui_state_.group_invite_policy = bindings_.group_invite_policy.c_str();
  ui_state_.group_invite_policy_label = bindings_.group_invite_policy_label.c_str();
  ui_state_.call_invite_policy = bindings_.call_invite_policy.c_str();
  ui_state_.call_invite_policy_label = bindings_.call_invite_policy_label.c_str();
  ui_state_.attachment_download_policy = bindings_.attachment_download_policy.c_str();
  ui_state_.attachment_download_policy_label = bindings_.attachment_download_policy_label.c_str();
  ui_state_.attachment_auto_on_wifi = bindings_.attachment_auto_on_wifi.c_str();
  ui_state_.attachment_auto_on_cellular = bindings_.attachment_auto_on_cellular.c_str();
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
  bindings_.backend_env = ui_state_.backend_env.c_str();
  bindings_.backend_label = BackendDisplayLabel(ui_state_.backend_env).c_str();
  bindings_.backend_restart_pending = ui_state_.backend_restart_pending;
  bindings_.backend_is_custom = ui_state_.backend_env == "custom";
  bindings_.direct_connections = ui_state_.direct_connections.c_str();
  bindings_.trusted_relays_only = ui_state_.trusted_relays_only.c_str();
  bindings_.reachable_without_relay = ui_state_.reachable_without_relay.c_str();
  bindings_.direct_connections_label = DirectConnectionsDisplayLabel(ui_state_.direct_connections).c_str();
  bindings_.show_prefer_contacts_toggle = ui_state_.show_prefer_contacts_toggle;
  bindings_.profile_nickname = ui_state_.profile_nickname.c_str();
  bindings_.profile_peer_id = ui_state_.profile_peer_id.c_str();
  bindings_.profile_relay_id = ui_state_.profile_relay_id.c_str();
  bindings_.profile_registered = ui_state_.profile_registered.c_str();
  bindings_.profile_registration_status = ui_state_.profile_registration_status.c_str();
  bindings_.profile_registration_expires = ui_state_.profile_registration_expires.c_str();
  bindings_.profile_register_label = ui_state_.profile_register_label.c_str();
  bindings_.profile_show_register = ui_state_.profile_show_register;
  bindings_.profile_register_primary = ui_state_.profile_register_primary;
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
  bindings_.show_notification_preview = ui_state_.show_notification_preview.c_str();
  bindings_.stay_connected = ui_state_.stay_connected.c_str();
  bindings_.brief_llm_key_masked = ui_state_.brief_llm_key_masked.c_str();
  bindings_.appearance = ui_state_.appearance.c_str();
  bindings_.appearance_label = ui_state_.appearance_label.c_str();
  bindings_.language = ui_state_.language.c_str();
  bindings_.language_label = ui_state_.language_label.c_str();
  bindings_.reduce_transparency = ui_state_.reduce_transparency.c_str();
  bindings_.call_diagnostics = ui_state_.call_diagnostics.c_str();
  bindings_.home_show_dynamic = ui_state_.home_show_dynamic.c_str();
  bindings_.home_animated_logo = ui_state_.home_animated_logo.c_str();
  bindings_.crash_reports_enabled = ui_state_.crash_reports_enabled.c_str();
  bindings_.show_advanced_settings = ui_state_.show_advanced_settings.c_str();
  bindings_.show_tester_options = ui_state_.show_tester_options.c_str();
  bindings_.advanced = ui_state_.show_advanced_settings == "on";
  bindings_.tester = ui_state_.show_tester_options == "on";
  bindings_.profile_label = ui_state_.profile_label.c_str();
  bindings_.config_dir = ui_state_.config_dir.c_str();
  bindings_.data_dir = ui_state_.data_dir.c_str();
  bindings_.profile_dir = ui_state_.profile_dir.c_str();
  bindings_.attachment_download_policy = ui_state_.attachment_download_policy.c_str();
  bindings_.attachment_download_policy_label = ui_state_.attachment_download_policy_label.c_str();
  bindings_.attachment_auto_on_wifi = ui_state_.attachment_auto_on_wifi.c_str();
  bindings_.attachment_auto_on_cellular = ui_state_.attachment_auto_on_cellular.c_str();
  bindings_.attachment_auto_toggles_visible = AttachmentAutoTogglesVisible(ui_state_, Platform::Detect());
  bindings_.pin_protection_status = ui_state_.pin_protection_status.c_str();
  bindings_.pin_protection_is_default = ui_state_.pin_protection_is_default;
  bindings_.pin_protection_not_setup = ui_state_.pin_protection_not_setup;
  bindings_.security_can_change_pin = ui_state_.security_can_change_pin;
  bindings_.security_can_export_link = ui_state_.security_can_export_link;
  bindings_.security_has_recovery_phrase = ui_state_.security_has_recovery_phrase;
  bindings_.unlock_grace_minutes = ui_state_.unlock_grace_minutes;
  bindings_.unlock_grace_label = ui_state_.unlock_grace_label.c_str();
  bindings_.security_grace_shown = ui_state_.security_grace_shown;
  bindings_.security_biometric_shown = ui_state_.security_biometric_shown;
  bindings_.security_biometric_on = ui_state_.security_biometric_on;
  bindings_.security_biometric_label = ui_state_.security_biometric_label.c_str();
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
  if (const SettingsPage* page = FindSettingsPage(selected_id_.c_str())) {
    if (SettingsPageHas(*page, SettingsPageAction::RefreshCasLibrary)) {
      cas_library_.Refresh();
    }
    if (SettingsPageHas(*page, SettingsPageAction::RefreshDevices)) {
      own_devices_.Refresh();
    }
  }

  const BootstrapResult& bootstrap = Store().Snapshot();
  for (const std::unique_ptr<SettingsSectionHandler>& handler : section_handlers_) {
    handler->SyncFromSession(bootstrap, ui_state_);
  }
  ai_dot_shown_ = bootstrap.profile_prefs.ai_dot_shown;
  ai_dot_look_ = bootstrap.profile_prefs.ai_dot_look.c_str();
  ai_dot_size_ = bootstrap.profile_prefs.ai_dot_size.c_str();
  ai_dot_size_label_ = Tr("settings.ai_dot.size_" + bootstrap.profile_prefs.ai_dot_size).c_str();
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
      ui_state_.profile_nickname = edits.ResolveAfterLoad(kUiFieldProfileNickname, view.nickname, live);
      if (!edits.IsMidEdit(kUiFieldProfileNickname, live)) {
        edits.OnLoaded(kUiFieldProfileNickname, view.nickname);
      }
    }
  } else if (const std::string* baseline = edits.Baseline(kUiFieldProfileNickname)) {
    ui_state_.profile_nickname = edits.ResolveAfterLoad(kUiFieldProfileNickname, *baseline, live);
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
      section_handle.RegisterMember("group", &SectionListRow::group);
      section_handle.RegisterMember("attention", &SectionListRow::attention);
    }
    if (auto mcp_handle = ctor.RegisterStruct<McpServerRow>()) {
      mcp_handle.RegisterMember("id", &McpServerRow::id);
      mcp_handle.RegisterMember("url", &McpServerRow::url);
      mcp_handle.RegisterMember("command", &McpServerRow::command);
      mcp_handle.RegisterMember("args_text", &McpServerRow::args_text);
      mcp_handle.RegisterMember("enabled", &McpServerRow::enabled);
    }
    if (auto option_handle = ctor.RegisterStruct<SettingsChoicePage::Option>()) {
      option_handle.RegisterMember("id", &SettingsChoicePage::Option::id);
      option_handle.RegisterMember("label", &SettingsChoicePage::Option::label);
      option_handle.RegisterMember("selected", &SettingsChoicePage::Option::selected);
    }
    ctor.RegisterArray<std::vector<SettingsChoicePage::Option>>();
    ctor.RegisterArray<std::vector<SectionListRow>>();
    ctor.RegisterArray<std::vector<McpServerRow>>();
    ctor.Bind("sections", &controller.sections_);
    ctor.Bind("selected_id", &controller.selected_id_);
    ctor.Bind("selected_title", &controller.selected_title_);
    ctor.Bind("show_detail", &controller.show_detail_);
    ctor.Bind("security_page", &controller.security_page_);
    ctor.Bind("security_page_title", &controller.security_page_title_);
    ctor.Bind("choice_options", &controller.choice_page_.Options());
    ctor.Bind("choice_foot", &controller.choice_foot_);
    ctor.Bind("choice_help_topic", &controller.choice_help_topic_);
    ctor.Bind("blocked_count", &controller.blocked_count_);
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
    controller.bindings_.show_backend_switch = BackendSwitchVisible(kUiDemoBuild);
    ctor.Bind("show_backend_switch", &controller.bindings_.show_backend_switch);
    ctor.Bind("backend_label", &controller.bindings_.backend_label);
    ctor.Bind("backend_restart_pending", &controller.bindings_.backend_restart_pending);
    ctor.Bind("backend_is_custom", &controller.bindings_.backend_is_custom);
    ctor.Bind("direct_connections_label", &controller.bindings_.direct_connections_label);
    ctor.Bind("trusted_relays_only", &controller.bindings_.trusted_relays_only);
    ctor.Bind("reachable_without_relay", &controller.bindings_.reachable_without_relay);
    ctor.Bind("show_prefer_contacts_toggle", &controller.bindings_.show_prefer_contacts_toggle);
    ctor.Bind("profile_nickname", &controller.bindings_.profile_nickname);
    ctor.Bind("profile_peer_id", &controller.bindings_.profile_peer_id);
    ctor.Bind("profile_relay_id", &controller.bindings_.profile_relay_id);
    ctor.Bind("profile_registered", &controller.bindings_.profile_registered);
    ctor.Bind("profile_registration_status", &controller.bindings_.profile_registration_status);
    ctor.Bind("profile_registration_expires", &controller.bindings_.profile_registration_expires);
    ctor.Bind("profile_register_label", &controller.bindings_.profile_register_label);
    ctor.Bind("profile_show_register", &controller.bindings_.profile_show_register);
    ctor.Bind("profile_register_primary", &controller.bindings_.profile_register_primary);
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
    ctor.Bind("show_notification_preview", &controller.bindings_.show_notification_preview);
    ctor.Bind("stay_connected", &controller.bindings_.stay_connected);
    controller.bindings_.stay_connected_available = Platform::Detect() == PlatformKind::Android;
    ctor.Bind("stay_connected_available", &controller.bindings_.stay_connected_available);
    ctor.Bind("call_banner_tip_available", &controller.bindings_.call_banner_tip_available);
    ctor.Bind("tip_notifications_blocked", &controller.bindings_.tip_notifications_blocked);
    ctor.Bind("tip_silenced", &controller.bindings_.tip_silenced);
    ctor.Bind("tip_battery", &controller.bindings_.tip_battery);
    ctor.Bind("tip_autostart", &controller.bindings_.tip_autostart);
    ctor.Bind("stay_help_plain", &controller.bindings_.stay_help_plain);
    ctor.Bind("stay_help_build", &controller.bindings_.stay_help_build);
    ctor.Bind("stay_help_no_google", &controller.bindings_.stay_help_no_google);
    controller.bindings_.dev_tools_available = kDevToolsBuild;
    ctor.Bind("dev_tools_available", &controller.bindings_.dev_tools_available);
    ctor.Bind("brief_llm_key_masked", &controller.bindings_.brief_llm_key_masked);
    ctor.Bind("appearance", &controller.bindings_.appearance);
    ctor.Bind("appearance_label", &controller.bindings_.appearance_label);
    ctor.Bind("language", &controller.bindings_.language);
    ctor.Bind("language_label", &controller.bindings_.language_label);
    ctor.Bind("reduce_transparency", &controller.bindings_.reduce_transparency);
    ctor.Bind("call_diagnostics", &controller.bindings_.call_diagnostics);
    ctor.Bind("home_show_dynamic", &controller.bindings_.home_show_dynamic);
    ctor.Bind("home_animated_logo", &controller.bindings_.home_animated_logo);
    ctor.Bind("crash_reports_enabled", &controller.bindings_.crash_reports_enabled);
    ctor.Bind("show_advanced_settings", &controller.bindings_.show_advanced_settings);
    ctor.Bind("advanced", &controller.bindings_.advanced);
    ctor.Bind("show_tester_options", &controller.bindings_.show_tester_options);
    ctor.Bind("tester", &controller.bindings_.tester);
    ctor.Bind("profile_label", &controller.bindings_.profile_label);
    ctor.Bind("config_dir", &controller.bindings_.config_dir);
    ctor.Bind("data_dir", &controller.bindings_.data_dir);
    ctor.Bind("profile_dir", &controller.bindings_.profile_dir);
    ctor.Bind("attachment_download_policy", &controller.bindings_.attachment_download_policy);
    ctor.Bind("attachment_download_policy_label", &controller.bindings_.attachment_download_policy_label);
    ctor.Bind("attachment_auto_on_wifi", &controller.bindings_.attachment_auto_on_wifi);
    ctor.Bind("attachment_auto_on_cellular", &controller.bindings_.attachment_auto_on_cellular);
    ctor.Bind("attachment_auto_toggles_visible", &controller.bindings_.attachment_auto_toggles_visible);
    ctor.Bind("pin_protection_status", &controller.bindings_.pin_protection_status);
    ctor.Bind("pin_protection_is_default", &controller.bindings_.pin_protection_is_default);
    ctor.Bind("pin_protection_not_setup", &controller.bindings_.pin_protection_not_setup);
    ctor.Bind("security_can_change_pin", &controller.bindings_.security_can_change_pin);
    ctor.Bind("security_can_export_link", &controller.bindings_.security_can_export_link);
    ctor.Bind("security_has_recovery_phrase", &controller.bindings_.security_has_recovery_phrase);
    ctor.Bind("unlock_grace_label", &controller.bindings_.unlock_grace_label);
    ctor.Bind("security_grace_shown", &controller.bindings_.security_grace_shown);
    ctor.Bind("security_biometric_shown", &controller.bindings_.security_biometric_shown);
    ctor.Bind("security_biometric_on", &controller.bindings_.security_biometric_on);
    ctor.Bind("security_biometric_label", &controller.bindings_.security_biometric_label);
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
    storage_manager_.Bind(ctor);
    ctor.Bind("status", &controller.status_);
    ctor.Bind("ai_dot_shown", &controller.ai_dot_shown_);
    ctor.Bind("ai_dot_look", &controller.ai_dot_look_);
    ctor.Bind("ai_dot_size_label", &controller.ai_dot_size_label_);
    ctor.BindEventCallback("select_section",
                           DataEvent<std::string>([this](const std::string& id) { OnSelectSection(id); }));
    ctor.BindEventCallback("back_to_list", DataEvent<>([this]() { OnBackToList(); }));
    ctor.BindEventCallback("open_security_page",
                           DataEvent<std::string>([this](const std::string& page) { OpenSecurityPage(page); }));
    ctor.BindEventCallback("close_security_page", DataEvent<>([this]() { OnCloseSecurityPage(); }));
    ctor.BindEventCallback("pick_choice", DataEvent<std::string>([this](const std::string& id) { OnPickChoice(id); }));
    ctor.BindEventCallback("open_blocked_people", DataEvent<>([this]() {
                             if (commands_.open_blocked_people) {
                               commands_.open_blocked_people();
                             }
                           }));
    ctor.BindEventCallback("reset_section",
                           DataEvent<std::string>([this](const std::string& id) { OnResetSection(id); }));
    ctor.BindEventCallback("open_support_chat", DataEvent<>([this]() { OnOpenSupportChat(); }));
    ctor.BindEventCallback("ask_setting_help", [this](ui::DataModelHandle, ui::Event& ev, const ui::VariantList& args) {
      ev.StopPropagation(); // the "?" can sit inside a row that opens a page or toggles a switch
      if (const auto key = data_event_detail::Arg<std::string>(args, 0)) {
        OnAskSettingHelp(*key);
      }
    });
    ctor.BindEventCallback("on_llm_field_changed", DataEvent<>([this]() { MarkSectionDirty("llm"); }));
    ctor.BindEventCallback("on_llm_preset_changed", DataEventAt([this](ui::Event& ev) { OnLlmPresetChanged(ev); }));
    ctor.BindEventCallback("on_choose_theme", DataEventAt([this](ui::Event& ev) { OnChooseTheme(ev); }));
    ctor.BindEventCallback("on_choose_ai_dot_size", DataEventAt([this](ui::Event& ev) { OnChooseAiDotSize(ev); }));
    ctor.BindEventCallback("on_choose_language", DataEventAt([this](ui::Event& ev) { OnChooseLanguage(ev); }));
    ctor.BindEventCallback("on_choose_group_invite_policy",
                           DataEventAt([this](ui::Event& ev) { OnChooseGroupInvitePolicy(ev); }));
    ctor.BindEventCallback("on_choose_call_invite_policy",
                           DataEventAt([this](ui::Event& ev) { OnChooseCallInvitePolicy(ev); }));
    ctor.BindEventCallback("on_choose_unlock_grace", DataEventAt([this](ui::Event& ev) { OnChooseUnlockGrace(ev); }));
    ctor.BindEventCallback("on_choose_backend", DataEventAt([this](ui::Event& ev) { OnChooseBackend(ev); }));
    ctor.BindEventCallback("on_choose_direct_connections",
                           DataEventAt([this](ui::Event& ev) { OnChooseDirectConnections(ev); }));
    ctor.BindEventCallback("toggle_attachment_auto_wifi",
                           DataEvent<>([this]() { ToggleAttachmentAutoNetwork(false); }));
    ctor.BindEventCallback("toggle_attachment_auto_cellular",
                           DataEvent<>([this]() { ToggleAttachmentAutoNetwork(true); }));
    ctor.BindEventCallback(
        "choose_attachment_download_policy",
        DataEvent<std::string>([this](const std::string& policy) { ApplyAttachmentDownloadPolicyChoice(policy); }));
    ctor.BindEventCallback("toggle_show_notifications", DataEvent<>([this]() { ToggleShowNotifications(); }));
    ctor.BindEventCallback("toggle_show_notification_preview",
                           DataEvent<>([this]() { ToggleShowNotificationPreview(); }));
    ctor.BindEventCallback("toggle_stay_connected", DataEvent<>([this]() { ToggleStayConnected(); }));
    ctor.BindEventCallback("open_notification_settings", DataEvent<>([]() { OpenNotificationSettings(); }));
    ctor.BindEventCallback("open_notification_fix", DataEvent<std::string>([](const std::string& fix) {
                             if (fix == "notifications") {
                               OpenNotificationFix(NotificationFix::AppNotifications);
                             } else if (fix == "silenced") {
                               OpenNotificationFix(NotificationFix::DoNotDisturb);
                             } else if (fix == "battery") {
                               OpenNotificationFix(NotificationFix::Battery);
                             } else if (fix == "autostart") {
                               OpenNotificationFix(NotificationFix::Autostart);
                             }
                           }));
    ctor.BindEventCallback("toggle_transparency", DataEvent<>([this]() { ToggleTransparency(); }));
    ctor.BindEventCallback("toggle_call_diagnostics", DataEvent<>([this]() { ToggleCallDiagnostics(); }));
    ctor.BindEventCallback("show_ask_ai_intro_again", DataEvent<>([this]() { ShowAskAiIntroAgain(); }));
    ctor.BindEventCallback("toggle_home_show_dynamic", DataEvent<>([this]() { ToggleHomeShowDynamic(); }));
    ctor.BindEventCallback("toggle_home_animated_logo", DataEvent<>([this]() { ToggleHomeAnimatedLogo(); }));
    ctor.BindEventCallback("open_ui_perf_test", DataEvent<>([this]() {
                             if constexpr (kDevToolsBuild) {
                               if (open_ui_perf_test_) {
                                 open_ui_perf_test_();
                               }
                             }
                           }));
    for (const char* state : {"offline", "connecting", "online"}) {
      ctor.BindEventCallback(std::string("simulate_chats_link_") + state, DataEvent<>([this, state]() {
                               (void)state;
                               if constexpr (kDevToolsBuild) {
                                 if (simulate_chats_link_) {
                                   simulate_chats_link_(state);
                                 }
                               }
                             }));
    }
    ctor.BindEventCallback("open_home_edit", DataEvent<>([this]() {
                             if (open_home_edit_) {
                               open_home_edit_();
                             }
                           }));
    ctor.BindEventCallback("tap_version", DataEvent<>([this]() { OnTapVersion(); }));
    ctor.BindEventCallback("toggle_advanced_settings", DataEvent<>([this]() { ToggleShowAdvancedSettings(); }));
    ctor.BindEventCallback("open_terms", DataEvent<>([]() { UserFeedback::Ok(Tr("settings.help.coming_soon")); }));
    ctor.BindEventCallback("open_privacy_policy",
                           DataEvent<>([]() { UserFeedback::Ok(Tr("settings.help.coming_soon")); }));
    ctor.BindEventCallback("toggle_ai_dot", DataEvent<>([this]() { ToggleAiDot(); }));
    ctor.BindEventCallback("set_ai_dot_look",
                           DataEvent<std::string>([this](const std::string& look) { SetAiDotLook(look); }));
    ctor.BindEventCallback("toggle_crash_reports", DataEvent<>([this]() { ToggleCrashReports(); }));
    ctor.BindEventCallback("toggle_auto_renew_registration", DataEvent<>([this]() { ToggleAutoRenewRegistration(); }));
    ctor.BindEventCallback("on_integrations_field_changed",
                           DataEvent<>([this]() { MarkSectionDirty("integrations"); }));
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
    ctor.BindEventCallback("on_profile_nickname_commit",
                           DataEvent<>([this]() { CommitProfileNickname(/*show_toast=*/false); }));
    ctor.BindEventCallback("edit_profile_nickname", DataEvent<>([this]() { EditProfileNickname(); }));
    ctor.BindEventCallback("add_mcp_server", DataEvent<>([this]() { OnAddMcpServer(); }));
    ctor.BindEventCallback("remove_mcp_server", DataEvent<int>([this](int index) { OnRemoveMcpServer(index); }));
  });
}

void SettingsController::DirtyAll(bool include_profile_nickname) {
  auto& host = DataModelHost::Instance();
  const std::string live = bindings_.profile_nickname.c_str();
  const bool push_nick =
      include_profile_nickname && UiEditSession::Instance().ShouldPushToView(kUiFieldProfileNickname, live);
  host.Dirty("settings", "sections");
  host.Dirty("settings", "selected_id");
  host.Dirty("settings", "selected_title");
  host.Dirty("settings", "show_detail");
  host.Dirty("settings", "security_page");
  host.Dirty("settings", "security_page_title");
  host.Dirty("settings", "choice_options");
  host.Dirty("settings", "choice_foot");
  host.Dirty("settings", "choice_help_topic");
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
  host.Dirty("settings", "backend_label");
  host.Dirty("settings", "backend_restart_pending");
  host.Dirty("settings", "backend_is_custom");
  host.Dirty("settings", "direct_connections_label");
  host.Dirty("settings", "trusted_relays_only");
  host.Dirty("settings", "reachable_without_relay");
  host.Dirty("settings", "show_prefer_contacts_toggle");
  if (push_nick) {
    host.Dirty("settings", "profile_nickname");
  }
  host.Dirty("settings", "profile_peer_id");
  host.Dirty("settings", "profile_relay_id");
  host.Dirty("settings", "profile_registered");
  host.Dirty("settings", "profile_registration_status");
  host.Dirty("settings", "profile_registration_expires");
  host.Dirty("settings", "profile_register_label");
  host.Dirty("settings", "profile_show_register");
  host.Dirty("settings", "profile_register_primary");
  host.Dirty("settings", "profile_show_rotate");
  host.Dirty("settings", "profile_icon_src");
  host.Dirty("settings", "profile_has_icon");
  host.Dirty("settings", "profile_icon_uploading");
  host.Dirty("settings", "profile_show_clear_icon");
  host.Dirty("settings", "profile_avatar_letter");
  host.Dirty("settings", "profile_avatar_tone");
  host.Dirty("settings", "auto_renew_registration");
  host.Dirty("settings", "show_notifications");
  host.Dirty("settings", "show_notification_preview");
  host.Dirty("settings", "stay_connected");
  host.Dirty("settings", "call_banner_tip_available");
  host.Dirty("settings", "tip_notifications_blocked");
  host.Dirty("settings", "tip_silenced");
  host.Dirty("settings", "tip_battery");
  host.Dirty("settings", "tip_autostart");
  host.Dirty("settings", "stay_help_plain");
  host.Dirty("settings", "stay_help_build");
  host.Dirty("settings", "stay_help_no_google");
  host.Dirty("settings", "brief_llm_key_masked");
  host.Dirty("settings", "appearance");
  host.Dirty("settings", "appearance_label");
  host.Dirty("settings", "language");
  host.Dirty("settings", "language_label");
  host.Dirty("settings", "reduce_transparency");
  host.Dirty("settings", "call_diagnostics");
  host.Dirty("settings", "home_show_dynamic");
  host.Dirty("settings", "home_animated_logo");
  host.Dirty("settings", "crash_reports_enabled");
  host.Dirty("settings", "show_advanced_settings");
  host.Dirty("settings", "ai_dot_shown");
  host.Dirty("settings", "ai_dot_look");
  host.Dirty("settings", "ai_dot_size_label");
  host.Dirty("settings", "advanced");
  host.Dirty("settings", "show_tester_options");
  host.Dirty("settings", "tester");
  host.Dirty("settings", "profile_label");
  host.Dirty("settings", "config_dir");
  host.Dirty("settings", "data_dir");
  host.Dirty("settings", "profile_dir");
  host.Dirty("settings", "attachment_download_policy");
  host.Dirty("settings", "attachment_download_policy_label");
  host.Dirty("settings", "attachment_auto_on_wifi");
  host.Dirty("settings", "attachment_auto_on_cellular");
  host.Dirty("settings", "attachment_auto_toggles_visible");
  host.Dirty("settings", "pin_protection_status");
  host.Dirty("settings", "pin_protection_is_default");
  host.Dirty("settings", "pin_protection_not_setup");
  host.Dirty("settings", "security_can_change_pin");
  host.Dirty("settings", "security_can_export_link");
  host.Dirty("settings", "security_has_recovery_phrase");
  host.Dirty("settings", "unlock_grace_label");
  host.Dirty("settings", "security_grace_shown");
  host.Dirty("settings", "security_biometric_shown");
  host.Dirty("settings", "security_biometric_on");
  host.Dirty("settings", "security_biometric_label");
  host.Dirty("settings", "recovery_phrase");
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
    if (chrome.nav_tab != NavTab::Me) {
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

bool SettingsController::SheetMountEmpty() const {
  if (!context_ || context_->GetNumDocuments() == 0) {
    return false;
  }
  ui::Element* mount = context_->GetDocument(0)->GetElementById("settings-section-mount-sheet");
  return mount && mount->GetNumChildren() == 0;
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
  const bool use_sheet_mount = show_detail_;
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
  const SettingsPage* page = FindSettingsPage(selected_id_.c_str());
  if (!page) {
    log().error << "No settings page '" << selected_id_.c_str() << "'";
    return;
  }
  const std::string key = page->view_key;
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
    // Phones too: a page pushed over the open section (Security > Blocked) and popped again remounts the shell.
    if (!selected_id_.empty() &&
        (chrome.settings_detail_primary || (show_detail_ && chrome.nav_tab == NavTab::Me && SheetMountEmpty()))) {
      suppress_auto_save_ = true;
      UiEditSession::Instance().BeginRemount();
      FinishPaneResync();
    }
    return;
  }
  if (chrome.nav_tab == NavTab::Me) {
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
  ResetSecurityPage();
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

void SettingsController::OnMeSurfaceClosed() {
  CommitProfileNickname(/*show_toast=*/false);
  FlushPending();
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
  ResetSecurityPage(); // desktop shows Change PIN / the transfer code inline

  if (chrome.nav_tab != NavTab::Me) {
    return;
  }
  if (compact) {
    // Me stays the selected tab; a detail open in the expanded primary pane becomes the inline detail.
    if (shell_navigation_.clear_primary_pane) {
      shell_navigation_.clear_primary_pane();
    }
    selected_id_ = saved_id;
    selected_title_ = saved_title;
    show_detail_ = had_detail;
    if (had_detail) {
      if (shell_navigation_.clear_local_back) {
        shell_navigation_.clear_local_back("settings_detail");
      }
      if (shell_navigation_.push_local_back) {
        shell_navigation_.push_local_back("settings_detail", [this] { ApplyBackToListUi(); });
      }
      storage_manager_.RestoreBackStack();
    }
    DirtyAll();
    return;
  }
  // Compact inline detail moves to the expanded primary pane.
  if (shell_navigation_.clear_local_back) {
    shell_navigation_.clear_local_back("settings_detail");
  }
  selected_id_ = saved_id;
  selected_title_ = saved_title;
  show_detail_ = false;
  if (had_detail && !selected_id_.empty()) {
    OpenSettingsDetailPane();
    storage_manager_.RestoreBackStack();
  }
  DirtyAll();
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
  if (section_id == "network" && commands_.set_backend_switch_pending) {
    const BootstrapResult& saved = Store().Snapshot();
    commands_.set_backend_switch_pending(SavedBackendKey(saved.config) != saved.backend_key);
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

void SettingsController::EditProfileNickname() {
  if (!shell_feedback_.show_prompt) {
    return;
  }
  // The saved name is what the dialog starts from; the binding only holds a name that was saved (or the desktop's
  // unsaved typing, which the phone page does not have).
  const std::string* baseline = UiEditSession::Instance().Baseline(kUiFieldProfileNickname);
  const std::string current = baseline ? *baseline : std::string(bindings_.profile_nickname.c_str());
  shell_feedback_.show_prompt(Tr("settings.nickname"), Tr("settings.profile.nickname_prompt"), current,
                              [this](const bool ok, const std::string& value) {
                                if (ok) {
                                  ApplyProfileNicknameFromDialog(value);
                                }
                              });
}

void SettingsController::ApplyProfileNicknameFromDialog(const std::string& value) {
  auto& edits = UiEditSession::Instance();
  const std::string* baseline = edits.Baseline(kUiFieldProfileNickname);
  if (baseline && value == *baseline) {
    return;
  }
  // The same guards CommitProfileNickname has: it returns without a word, so ask first. Nothing is written to the
  // binding when the save cannot run, and the page keeps showing the saved name.
  if (!baseline || suppress_auto_save_ || edits.RemountBlocking()) {
    UserFeedback::Fail(Tr("settings.profile.nickname_not_saved"));
    return;
  }
  const std::string saved_name = *baseline;
  bindings_.profile_nickname = value.c_str();
  const bool flushed = FlushSection("profile", /*show_toast=*/true);
  // A successful save moved the baseline to the stored name and pushed it to the page (FlushSection -> DirtyAll).
  // Anything else leaves the unsaved name in the binding: put the saved one back. A failed flush has already told
  // the user why (ReportFailure); a flush that returned true without saving gets our own line.
  if (!flushed || edits.IsMidEdit(kUiFieldProfileNickname, bindings_.profile_nickname.c_str())) {
    bindings_.profile_nickname = saved_name.c_str();
    ui_state_.profile_nickname = saved_name;
    DirtyAll();
    if (flushed) {
      UserFeedback::Fail(Tr("settings.profile.nickname_not_saved"));
    }
  }
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

  if (!shell_feedback_.show_confirm_destructive) {
    return;
  }
  shell_feedback_.show_confirm_destructive(
      Tr("settings.reset_defaults_confirm_title"),
      Tr(section_id == "network" ? "settings.reset_defaults_confirm_message.network"
                                 : "settings.reset_defaults_confirm_message"),
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
  ResetSecurityPage();

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
  const SettingsPage* opened = FindSettingsPage(section_id);
  // Send problem logs is on the Help page: it reads its label again.
  if (opened && SettingsPageHas(*opened, SettingsPageAction::RefreshDiagnostics)) {
    security_actions_.RefreshDiagnostics();
  }
  if (opened && SettingsPageHas(*opened, SettingsPageAction::RefreshBlockedCount)) {
    RefreshBlockedCount();
  }
  if (opened && SettingsPageHas(*opened, SettingsPageAction::RefreshBannerTip)) {
    RefreshBannerTip();
    RefreshNotificationTips();
  }
  storage_manager_.Reset();
  if (opened && SettingsPageHas(*opened, SettingsPageAction::EnterStorage)) {
    storage_manager_.Enter();
  }
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (chrome.layout_mode == LayoutMode::Compact) {
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
  // Compact Me shows the detail inline, not in a transient pane.
  if (compact_layout_) {
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
  ResetSecurityPage();
  selected_id_.clear();
  selected_title_.clear();
  show_detail_ = false;
  status_ = "";
  DirtyAll();
}

void SettingsController::ApplyBackToListUi() {
  FlushPending();
  ResetSecurityPage();
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
  if (chrome.layout_mode == LayoutMode::Compact) {
    ApplyBackToListUi();
    return;
  }
  if (!CloseSettingsDetailPane()) {
    OnDetailDismissed();
  } else {
    OnDetailDismissed();
  }
}

void SettingsController::RefreshBlockedCount() {
  blocked_count_ = commands_.blocked_count ? commands_.blocked_count() : 0;
  DataModelHost::Instance().Dirty("settings", "blocked_count");
}

void SettingsController::OpenSecurityPage(const std::string& page) {
  if (page == "change_pin") {
    OpenChangePin(); // B2: the PIN gate's three steps, not a page of fields
    return;
  }
  if (selected_id_ != "devices" || !show_detail_) {
    return; // phones only: desktop shows both inline
  }
  if (page == "transfer") {
    security_page_title_ = Tr("settings.security.transfer_row").c_str();
  } else {
    return;
  }
  log().info << "OpenSecurityPage " << page;
  security_page_ = page.c_str();
  status_ = "";
  DirtyAll();
  if (context_) {
    context_->Update(); // the page must be in the DOM before the shell plays its push
  }
  if (shell_navigation_.push_local_back) {
    shell_navigation_.push_local_back("settings_subpage", [this] { ApplyCloseSecurityPage(); });
  }
  if (shell_navigation_.set_secret_page_open) {
    shell_navigation_.set_secret_page_open(true); // the transfer code is on it: no tab bar under it
  }
}

void SettingsController::OnCloseSecurityPage() {
  if (shell_navigation_.has_local_back && shell_navigation_.has_local_back("settings_subpage")) {
    if (shell_navigation_.request_dismiss_instant) {
      shell_navigation_.request_dismiss_instant();
    }
    return;
  }
  ApplyCloseSecurityPage();
}

void SettingsController::ApplyCloseSecurityPage() {
  if (shell_navigation_.set_secret_page_open) {
    shell_navigation_.set_secret_page_open(false);
  }
  security_page_ = "";
  security_page_title_ = "";
  choice_page_.Close();
  choice_foot_ = "";
  choice_help_topic_ = "";
  status_ = "";
  AppRuntime::PostUI([this]() {
    DirtyAll();
    if (context_) {
      context_->Update();
    }
    if (shell_navigation_.refresh_dismiss_gestures) {
      shell_navigation_.refresh_dismiss_gestures();
    }
  });
}

void SettingsController::ResetSecurityPage() {
  if (shell_navigation_.clear_local_back) {
    shell_navigation_.clear_local_back("settings_subpage");
  }
  if (shell_navigation_.set_secret_page_open) {
    shell_navigation_.set_secret_page_open(false);
  }
  security_actions_.HideRecoveryPhrase(); // the phrase does not follow the user off the page
  if (security_page_.empty()) {
    return;
  }
  security_page_ = "";
  security_page_title_ = "";
  choice_page_.Close();
  choice_foot_ = "";
  choice_help_topic_ = "";
  DataModelHost::Instance().Dirty("settings", "security_page");
  DataModelHost::Instance().Dirty("settings", "security_page_title");
  DataModelHost::Instance().Dirty("settings", "choice_options");
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

void SettingsController::ShowChoice(ui::Event& ev, const std::string& kind, const std::string& title,
                                    const std::string& foot, const std::string& help_topic,
                                    std::vector<SettingsChoicePage::Option> options,
                                    std::function<void(const std::string&)> apply) {
  if (!show_detail_ || !compact_layout_) {
    // The desktop: the float menu under the row's value, as before.
    const ui::Vector2i position = ChoiceRowMenuPosition(ev);
    std::vector<ContextMenuAction> actions;
    actions.reserve(options.size());
    for (const SettingsChoicePage::Option& option : options) {
      const std::string id = option.id.c_str();
      actions.push_back({.id = id,
                         .label = option.label.c_str(),
                         .enabled = {},
                         .run = [apply, id]() { apply(id); },
                         .icon = {},
                         .danger = false,
                         .selected = option.selected});
    }
    ContextMenuHost::Instance().ShowActions(position, std::move(actions));
    return;
  }
  // Phones (Kenneth 2026-11-04): a page pushed over the section, one row per value, the current one checked.
  log().info << "OpenChoicePage " << kind;
  ++choice_pop_generation_;
  choice_page_.Open(kind, title, foot, help_topic, std::move(options), std::move(apply));
  security_page_ = "choice";
  security_page_title_ = title.c_str();
  choice_foot_ = foot.c_str();
  choice_help_topic_ = help_topic.c_str();
  status_ = "";
  DirtyAll();
  if (context_) {
    context_->Update(); // the page must be in the DOM before the shell plays its push
  }
  if (shell_navigation_.push_local_back) {
    shell_navigation_.push_local_back("settings_subpage", [this] { ApplyCloseSecurityPage(); });
  }
}

void SettingsController::OnPickChoice(const std::string& id) {
  if (security_page_ != "choice" || !choice_page_.Pick(id)) {
    return;
  }
  // The value is applied (and its row's label updated) by now; the check shows on the picked row, then the page goes.
  DataModelHost::Instance().Dirty("settings", "choice_options");
  const uint64_t generation = ++choice_pop_generation_;
  // A short beat so the check is seen landing; a thread, so the wait also ends while nothing else wakes the loop.
  std::thread([this, generation]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(SettingsChoicePage::kPopDelayMs));
    AppRuntime::PostUI([this, generation]() {
      if (generation == choice_pop_generation_ && security_page_ == "choice") {
        OnCloseSecurityPage();
      }
    });
  }).detach();
}

void SettingsController::OnChooseTheme(ui::Event& ev) {
  const std::string current = bindings_.appearance.empty() ? "system" : std::string(bindings_.appearance.c_str());
  std::vector<SettingsChoicePage::Option> options;
  for (const char* id : {"system", "light", "dark"}) {
    options.push_back({id, ThemeDisplayLabel(id).c_str(), current == id});
  }
  ShowChoice(ev, "theme", Tr("settings.theme.label"), Tr("settings.theme.help"), {}, std::move(options),
             [this](const std::string& id) { ApplyThemeChoice(id); });
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
  if (!commands_.language_display_label || !commands_.available_locales) {
    log().warning << "OnChooseLanguage: locale ports not bound";
    return;
  }

  const std::string current = bindings_.language.empty() ? "system" : std::string(bindings_.language.c_str());
  std::vector<SettingsChoicePage::Option> options;
  options.push_back({"system", commands_.language_display_label("system").c_str(), current == "system"});
  for (const LocaleInfo& info : commands_.available_locales()) {
    options.push_back({info.tag.c_str(), commands_.language_display_label(info.tag).c_str(), current == info.tag});
  }
  ShowChoice(ev, "language", Tr("settings.language.label"), Tr("settings.language.help"), {}, std::move(options),
             [this](const std::string& id) { ApplyLanguageChoice(id); });
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

std::vector<SettingsChoicePage::Option> SettingsController::BackendChoiceOptions() const {
  const std::string current = bindings_.backend_env.c_str();
  std::vector<SettingsChoicePage::Option> options;
  for (const char* id : BackendChoicesOffered(ui_state_.backend_qa_available, ui_state_.show_tester_options == "on")) {
    options.push_back({id, BackendDisplayLabel(id).c_str(), current == id});
  }
  return options;
}

void SettingsController::OnChooseBackend(ui::Event& ev) {
  ShowChoice(ev, "backend", Tr("settings.network.backend"), Tr("settings.network.backend.help"), {},
             BackendChoiceOptions(), [this](const std::string& id) { ApplyBackendChoice(id); });
}

void SettingsController::ApplyBackendChoice(const std::string& env) {
  if (suppress_auto_save_) {
    return;
  }
  ui_state_.backend_env = env;
  PushUiStateToBindings();
  MarkSectionDirty("network");
  FlushPending();
  DirtyAll();
}

void SettingsController::OnChooseDirectConnections(ui::Event& ev) {
  const std::string current =
      bindings_.direct_connections.empty() ? "contacts" : std::string(bindings_.direct_connections.c_str());
  std::vector<SettingsChoicePage::Option> options;
  for (const mesh::DirectAudience audience : {mesh::DirectAudience::Everyone, mesh::DirectAudience::Contacts,
                                              mesh::DirectAudience::Friendly, mesh::DirectAudience::Nobody}) {
    const std::string id = mesh::DirectAudienceName(audience);
    options.push_back({id.c_str(), DirectConnectionsDisplayLabel(id).c_str(), current == id});
  }
  ShowChoice(ev, "direct_connections", Tr("settings.network.direct_connections"),
             Tr("settings.network.direct_connections.help"), "settings.network.direct_connections", std::move(options),
             [this](const std::string& id) { ApplyDirectConnectionsChoice(id); });
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
  const std::string current =
      bindings_.call_invite_policy.empty() ? "everyone" : std::string(bindings_.call_invite_policy.c_str());
  std::vector<SettingsChoicePage::Option> options;
  for (const char* id : {"everyone", "contacts_only", "nobody"}) {
    options.push_back({id, CallInvitePolicyDisplayLabel(id).c_str(), current == id});
  }
  ShowChoice(ev, "call_invite_policy", Tr("settings.security.calls"), Tr("settings.security.calls.help"), {},
             std::move(options), [this](const std::string& id) { ApplyCallInvitePolicyChoice(id); });
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

void SettingsController::OnChooseUnlockGrace(ui::Event& ev) {
  const int current = bindings_.unlock_grace_minutes;
  std::vector<SettingsChoicePage::Option> options;
  for (const int minutes : OfferedUnlockGraceMinutes()) {
    options.push_back({std::to_string(minutes).c_str(), UnlockGraceDisplayLabel(minutes).c_str(), current == minutes});
  }
  ShowChoice(ev, "unlock_grace", Tr("settings.security.grace.title"), Tr("settings.security.grace.help"), {},
             std::move(options), [this](const std::string& id) { ApplyUnlockGraceChoice(id); });
}

void SettingsController::ApplyUnlockGraceChoice(const std::string& minutes_id) {
  if (suppress_auto_save_) {
    return;
  }
  const int minutes = minutes_id == "0"                                  ? 0
                      : minutes_id == "5"                                ? 5
                      : minutes_id == "60"                               ? 60
                      : minutes_id == std::to_string(kUnlockGraceAlways) ? kUnlockGraceAlways
                                                                         : 15;
  bindings_.unlock_grace_minutes = minutes;
  bindings_.unlock_grace_label = UnlockGraceDisplayLabel(minutes).c_str();
  PullBindingsToUiState();
  MarkSectionDirty("security");
  FlushPending(); // the section saves it, then tells the app (set_grace_minutes)
  DirtyAll();
}

void SettingsController::OnChooseGroupInvitePolicy(ui::Event& ev) {
  const std::string current =
      bindings_.group_invite_policy.empty() ? "everyone" : std::string(bindings_.group_invite_policy.c_str());
  std::vector<SettingsChoicePage::Option> options;
  for (const char* id : {"everyone", "contacts_only", "nobody"}) {
    options.push_back({id, GroupInvitePolicyDisplayLabel(id).c_str(), current == id});
  }
  ShowChoice(ev, "group_invite_policy", Tr("settings.security.group_invites"),
             Tr("settings.security.group_invites.help"), {}, std::move(options),
             [this](const std::string& id) { ApplyGroupInvitePolicyChoice(id); });
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

void SettingsController::ApplyAttachmentDownloadPolicyChoice(const std::string& policy) {
  if (suppress_auto_save_) {
    return;
  }
  PullBindingsToUiState();
  ChooseAttachmentDownloadPolicy(ui_state_, policy);
  PushUiStateToBindings();
  MarkSectionDirty("storage");
  FlushPending();
  DirtyAll();
}

void SettingsController::ToggleAttachmentAutoNetwork(const bool cellular) {
  if (suppress_auto_save_) {
    return;
  }
  PullBindingsToUiState();
  pbr::ToggleAttachmentAutoNetwork(ui_state_, cellular);
  PushUiStateToBindings();
  MarkSectionDirty("storage");
  FlushPending();
  DirtyAll();
}

void SettingsController::RefreshLocalizedChrome() {
  SyncBindingsFromSession();
  InitSections(); // after the sync: the advanced flag comes from the profile prefs
  ApplySupportDiscovery();
  if (!selected_id_.empty()) {
    if (const SettingsPage* page = FindSettingsPage(selected_id_.c_str())) {
      selected_title_ = Tr(page->title_key).c_str();
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
  // Localised title first ("PP 客服" / "PP Support"); the server's display name only when the locale has none.
  const std::string title = Tr("support.entry.title");
  bindings_.support_display_name = title.empty() ? discovery->display_name.c_str() : title.c_str();
}

void SettingsController::OnOpenSupportChat() {
  if (!commands_.open_support_chat) {
    ReportFailure(Error("Support is not available").WithUser(Tr("settings.support.unavailable")));
    return;
  }
  if (auto opened = commands_.open_support_chat({}); !opened) {
    ReportFailure(opened.error());
  }
}

std::string SettingHelpQuestion(const std::string& label_key) {
  if (label_key.rfind("settings.", 0) != 0) {
    return {};
  }
  const std::string title = Tr(label_key);
  if (title.empty() || title == label_key) {
    return {};
  }
  return Tr("settings.help.question", {{"title", title}});
}

void SettingsController::OnAskSettingHelp(const std::string& label_key) {
  const std::string question = SettingHelpQuestion(label_key);
  if (question.empty()) {
    return;
  }
  // Kenneth 2026-11-04 (DEV/PLAN-ui-batch5-ask-ai.md 原话 11–13): on phones the AI answers it in the half-screen panel,
  // a one-off question with no earlier conversation; the panel keeps a "Ask PP Support" button for a person.
  if (commands_.ask_ai_once && commands_.ask_ai_once(question)) {
    return;
  }
  if (!commands_.ask_support) {
    ReportFailure(Error("Support is not available").WithUser(Tr("settings.support.unavailable")));
    return;
  }
  // The desktop (no panel yet): the question goes straight to PP 客服 and the user watches the answer come.
  if (auto asked = commands_.ask_support(question); !asked) {
    ReportFailure(asked.error());
  }
}

void SettingsController::OpenNetworkSettings() {
  OpenSection("advanced");
}

void SettingsController::OpenSection(const std::string& section_id) {
  const ShellChromeSnapshot chrome = ChromeSnapshot();
  if (chrome.nav_tab != NavTab::Me) {
    if (shell_navigation_.select_nav_tab) {
      shell_navigation_.select_nav_tab(NavTab::Me);
    }
    // OnNavTabActivated clears selection; open the page on the next UI turn.
    AppRuntime::PostUI([this, section_id]() { OnSelectSection(section_id); });
    return;
  }
  OnSelectSection(section_id);
}

void SettingsController::ApplyReachability() {
  const bool mesh_ready =
      commands_.mesh_ready ? commands_.mesh_ready() : (commands_.messaging_ready && commands_.messaging_ready());
  ui_state_.show_connection_card = ui_state_.show_node_toggle && ui_state_.node_enabled == "on" && mesh_ready;
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
  const std::string& acked = commands_.session_store().Snapshot().profile_prefs.reachability_nudge_acked_status;
  return ReachabilityNudgeActive(true, status_key, acked);
}

void SettingsController::ApplySectionAttention() {
  const bool network_attention = ComputeNetworkAttention();
  for (SectionListRow& section : sections_) {
    section.attention = section.id == "advanced" && network_attention;
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
  bindings_.circuit_relay_enabled = bindings_.circuit_relay_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleMediaRelay() {
  if (!bindings_.show_media_relay_toggle) {
    return;
  }
  bindings_.media_relay_enabled = bindings_.media_relay_enabled == "on" ? "off" : "on";
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
  bindings_.prefer_contacts_for_routing = bindings_.prefer_contacts_for_routing == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("network", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleReachableWithoutRelay() {
  bindings_.reachable_without_relay = bindings_.reachable_without_relay == "on" ? "off" : "on";
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
  bindings_.show_notifications = bindings_.show_notifications == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleShowNotificationPreview() {
  bindings_.show_notification_preview = bindings_.show_notification_preview == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleStayConnected() {
  bindings_.stay_connected = bindings_.stay_connected == "on" ? "off" : "on";
  ApplyNotificationTips(); // the auto-start and battery rows follow this switch
  PullBindingsToUiState();
  MarkSectionDirty("profile", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleTransparency() {
  // The switch reads "Transparency" (on by default): on is reduce_transparency off.
  bindings_.reduce_transparency = bindings_.reduce_transparency == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("appearance", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleCallDiagnostics() {
  bindings_.call_diagnostics = bindings_.call_diagnostics == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("appearance", /*show_toast=*/false);
  DirtyAll();
}

Roe<void> ClearAskAiIntroSeen(SessionStore& store) {
  ProfilePreferences prefs = store.Snapshot().profile_prefs;
  prefs.ai_panel_intro_seen = false;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  return store.SaveProfilePrefs(prefs);
}

void SettingsController::ShowAskAiIntroAgain() {
  if (!commands_.session_store) {
    return;
  }
  if (auto saved = ClearAskAiIntroSeen(Store()); !saved) {
    ReportFailure(saved.error());
    return;
  }
  UserFeedback::Ok(Tr("settings.tester.show_ask_ai_intro.done"));
}

void SettingsController::ToggleHomeAnimatedLogo() {
  // Phones only (Me → AI assistant): on draws the AI Home's logo as the animated mascot (the look picked below it), off
  // keeps the static app icon. Saved at once, like the AI dot's switches: ConfigApplyBridge hands the pref to the shell,
  // which swaps the logo on the next frame.
  if (!commands_.session_store) {
    return;
  }
  const bool animated = bindings_.home_animated_logo != "on";
  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  prefs.home_animated_logo = animated;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
    ReportFailure(saved.error());
    return;
  }
  bindings_.home_animated_logo = animated ? "on" : "off";
  PullBindingsToUiState();
  DataModelHost::Instance().Dirty("settings", "home_animated_logo");
}

void SettingsController::ToggleHomeShowDynamic() {
  // Phones only: on shows the 动态 widgets section on Home, off is the minimal AI-first Home. Saved through the
  // appearance section (SaveProfilePrefs), which the Home's prefs listener picks up to redraw.
  bindings_.home_show_dynamic = bindings_.home_show_dynamic == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("appearance", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleAiDot() {
  SaveAiDot(!ai_dot_shown_, ai_dot_look_.c_str(), ai_dot_size_.c_str());
}

void SettingsController::SetAiDotLook(const std::string& look) {
  if (!IsLookPref(look)) {
    return;
  }
  SaveAiDot(ai_dot_shown_, look, ai_dot_size_.c_str());
}

void SettingsController::OnChooseAiDotSize(ui::Event& ev) {
  const std::string current = ai_dot_size_.c_str();
  std::vector<SettingsChoicePage::Option> options;
  for (const char* id : {"s", "m", "l"}) {
    options.push_back({id, Tr(std::string("settings.ai_dot.size_") + id).c_str(), current == id});
  }
  ShowChoice(ev, "ai_dot_size", Tr("settings.ai_dot.size"), {}, {}, std::move(options),
             [this](const std::string& id) { SaveAiDot(ai_dot_shown_, ai_dot_look_.c_str(), id); });
}

void SettingsController::SaveAiDot(const bool shown, const std::string& look, const std::string& size) {
  if (!commands_.session_store) {
    return;
  }
  // Saved at once (no section flush): ConfigApplyBridge hands the new prefs to the shell, which shows, hides or
  // redraws the dot on the next frame.
  ProfilePreferences prefs = Store().Snapshot().profile_prefs;
  prefs.ai_dot_shown = shown;
  prefs.ai_dot_look = look;
  prefs.ai_dot_size = size;
  prefs.schema_version = ProfilePreferences::kSchemaVersion;
  if (auto saved = Store().SaveProfilePrefs(prefs); !saved) {
    ReportFailure(saved.error());
    return;
  }
  ai_dot_shown_ = shown;
  ai_dot_look_ = look.c_str();
  ai_dot_size_ = size.c_str();
  ai_dot_size_label_ = Tr("settings.ai_dot.size_" + size).c_str();
  DataModelHost::Instance().Dirty("settings", "ai_dot_shown");
  DataModelHost::Instance().Dirty("settings", "ai_dot_look");
  DataModelHost::Instance().Dirty("settings", "ai_dot_size_label");
}

void SettingsController::OnTapVersion() {
  const bool was_on = bindings_.show_tester_options == "on";
  const VersionTapCounter::Result tap = version_taps_.Tap(SDL_GetTicks());
  if (tap.action == VersionTapCounter::Action::Toggle) {
    ToggleShowTesterOptions();
    UserFeedback::Ok(Tr(was_on ? "settings.help.tester_off" : "settings.help.tester_on"));
  } else if (tap.action == VersionTapCounter::Action::Hint) {
    UserFeedback::Ok(
        Tr(was_on ? "settings.help.tap_close" : "settings.help.tap_open", {{"n", std::to_string(tap.taps_left)}}));
  }
}

void SettingsController::ToggleShowAdvancedSettings() {
  bindings_.show_advanced_settings = bindings_.show_advanced_settings == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("about", /*show_toast=*/false);
  InitSections(); // the advanced-only sections appear or go
  PushUiStateToBindings();
  DirtyAll();
}

void SettingsController::ToggleShowTesterOptions() {
  bindings_.show_tester_options = bindings_.show_tester_options == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("about", /*show_toast=*/false);
  InitSections(); // the Test options page appears or goes
  PushUiStateToBindings();
  DirtyAll();
}

void SettingsController::ToggleCrashReports() {
  bindings_.crash_reports_enabled = bindings_.crash_reports_enabled == "on" ? "off" : "on";
  PullBindingsToUiState();
  MarkSectionDirty("security", /*show_toast=*/false);
  DirtyAll();
}

void SettingsController::ToggleAutoRenewRegistration() {
  bindings_.auto_renew_registration = bindings_.auto_renew_registration == "auto" ? "off" : "auto";
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

void SettingsController::OnProfileUnlocked() {
  Resync(/*include_profile_nickname=*/true);
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

void SettingsController::OnBiometricLost() {
  bindings_.security_biometric_on = false;
  ui_state_.security_biometric_on = false;
  DataModelHost::Instance().Dirty("settings", "security_biometric_on");
}

void SettingsController::OpenChangePin() {
  security_actions_.ChangePin();
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
