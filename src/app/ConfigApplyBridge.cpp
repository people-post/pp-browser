#include "app/ConfigApplyBridge.h"

#include "base/ui/Theme.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

namespace pbr {

void ConfigApplyBridge::Bind(MessagingHub& messaging, SessionStore& store, ShellHost& shell,
                             AssetPathResolver resolve_asset) {
  messaging_ = &messaging;
  store_ = &store;
  shell_ = &shell;
  resolve_asset_ = std::move(resolve_asset);
}

void ConfigApplyBridge::InstallListeners() {
  if (!messaging_ || !store_ || !store_->IsInitialized()) {
    return;
  }

  const BootstrapResult& snap = store_->Snapshot();
  last_network_ = MessagingHub::ProjectNetwork(snap.config);
  last_policy_ = MessagingHub::ProjectPolicy(snap.profile_prefs);
  last_notifications_ = MessagingHub::ProjectNotifications(snap.profile_prefs);
  last_chrome_ = ShellHost::ProjectChrome(snap.profile_prefs);
  last_locale_ = LocalizationService::Project(snap.profile_prefs);
  last_agent_ = ChatController::ProjectAgent(snap.config);

  store_->AddConfigListener([this](const AppConfig& config) { OnConfig(config); });
  store_->AddProfilePrefsListener([this](const ProfilePreferences& prefs) { OnProfilePrefs(prefs); });
}

void ConfigApplyBridge::OnConfig(const AppConfig& config) {
  if (!messaging_) {
    return;
  }
  const MessagingHub::NetworkConfig network = MessagingHub::ProjectNetwork(config);
  if (!last_network_ || network != *last_network_) {
    last_network_ = network;
    messaging_->Apply(network);
  }

  const ChatController::AgentConfig agent = ChatController::ProjectAgent(config);
  if (!last_agent_ || agent != *last_agent_) {
    last_agent_ = agent;
    ChatController::Instance().Apply(agent);
  }
}

void ConfigApplyBridge::OnProfilePrefs(const ProfilePreferences& prefs) {
  if (!messaging_) {
    return;
  }

  const MessagingHub::PolicyPrefs policy = MessagingHub::ProjectPolicy(prefs);
  if (!last_policy_ || policy != *last_policy_) {
    last_policy_ = policy;
    messaging_->Apply(policy);
  }

  const MessagingHub::NotificationPrefs notifications = MessagingHub::ProjectNotifications(prefs);
  if (!last_notifications_ || notifications != *last_notifications_) {
    last_notifications_ = notifications;
    messaging_->Apply(notifications);
  }

  const ShellHost::ChromePrefs chrome = ShellHost::ProjectChrome(prefs);
  if (!last_chrome_ || chrome != *last_chrome_) {
    const ShellHost::ChromePrefs* previous = last_chrome_ ? &*last_chrome_ : nullptr;
    last_chrome_ = chrome;
    ApplyChrome(chrome, previous);
  }

  const LocalizationService::Prefs locale = LocalizationService::Project(prefs);
  if (!last_locale_ || locale != *last_locale_) {
    last_locale_ = locale;
    LocalizationService::Instance().Apply(locale);
  }
}

void ConfigApplyBridge::ApplyChrome(const ShellHost::ChromePrefs& next,
                                    const ShellHost::ChromePrefs* previous) {
  const bool theme_changed = !previous || next.theme != previous->theme;
  const bool appearance_changed = !previous || next.appearance != previous->appearance;
  const bool material_changed =
      !previous || next.reduce_transparency != previous->reduce_transparency ||
      next.compact_chrome_frost != previous->compact_chrome_frost;

  if (theme_changed && resolve_asset_) {
    Theme::LoadBase(resolve_asset_(next.theme));
    if (auto* ctx = Rml::GetContext("main")) {
      if (ctx->GetNumDocuments() > 0) {
        ctx->GetDocument(0)->UpdateDocument();
      }
    }
  }
  if (appearance_changed) {
    if (auto* ctx = Rml::GetContext("main")) {
      Theme::ApplyAppearance(ctx, Theme::ParseAppearance(next.appearance));
    }
  }
  if (material_changed && shell_) {
    shell_->Apply(next);
  }
}

} // namespace pbr
