#include "app/ConfigApplyBridge.h"

#include "feature/product/HubSettingsProjection.h"
#include "foundation/data/BackendProfile.h"
#include "foundation/platform/ILocalNotifier.h"

#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "lib/ui/Theme.h"

#include <ui/dom/Context.h>
#include <ui/Core.h>
#include <ui/dom/ElementDocument.h>

namespace pbr {

void ConfigApplyBridge::Bind(ConversationsHub& messaging, SessionStore& store, ShellHost& shell, ChatController& chat,
                             AssetPathResolver resolve_asset) {
  messaging_ = &messaging;
  store_ = &store;
  shell_ = &shell;
  chat_ = &chat;
  resolve_asset_ = std::move(resolve_asset);
}

void ConfigApplyBridge::InstallListeners() {
  if (!messaging_ || !store_ || !store_->IsInitialized()) {
    return;
  }

  const BootstrapResult& snap = store_->Snapshot();
  last_network_ = ProjectNetwork(snap.config);
  last_policy_ = ProjectPolicy(snap.profile_prefs);
  last_notifications_ = ProjectNotifications(snap.profile_prefs);
  messaging_->Apply(*last_notifications_); // stay connected (P011) holds from unlock
  // The platform notifier (installed by PlatformHooks::Register, long before this) starts at its own default;
  // a saved "off" must reach it now, not only on the next change.
  ApplyMessageBanners(ILocalNotifier::Instance(), *last_notifications_);
  last_chrome_ = ShellHost::ProjectChrome(snap.profile_prefs);
  last_locale_ = LocalizationService::Prefs{.language = "en"}; // English only for now: see OnProfilePrefs
  last_agent_ = ChatController::ProjectAgent(snap.config);

  store_->AddConfigListener([this](const AppConfig& config) { OnConfig(config); });
  store_->AddProfilePrefsListener([this](const ProfilePreferences& prefs) { OnProfilePrefs(prefs); });
}

void ConfigApplyBridge::OnConfig(const AppConfig& config) {
  if (!messaging_) {
    return;
  }
  ConversationsHub::NetworkConfig network = ProjectNetwork(config);
  // A switch awaits a restart (another backend, or a custom backend's registration URL or org relay edited: PM010,
  // #745): its addresses are the next run's profile's, never applied to this one. The rest of the slice applies.
  if (last_network_ && BackendSwitchPending(config, store_->Snapshot().backend_key)) {
    network.relay = last_network_->relay;
    network.directory = last_network_->directory;
    network.registration = last_network_->registration;
  }
  if (!last_network_ || network != *last_network_) {
    last_network_ = network;
    messaging_->Apply(network);
  }

  const ChatController::AgentConfig agent = ChatController::ProjectAgent(config);
  if (!last_agent_ || agent != *last_agent_) {
    last_agent_ = agent;
    if (chat_) {
      chat_->Apply(agent);
    }
  }
}

void ConfigApplyBridge::OnProfilePrefs(const ProfilePreferences& prefs) {
  if (!messaging_) {
    return;
  }

  const ConversationsHub::PolicyPrefs policy = ProjectPolicy(prefs);
  if (!last_policy_ || policy != *last_policy_) {
    last_policy_ = policy;
    messaging_->Apply(policy);
  }

  const ConversationsHub::NotificationPrefs notifications = ProjectNotifications(prefs);
  if (!last_notifications_ || notifications != *last_notifications_) {
    last_notifications_ = notifications;
    messaging_->Apply(notifications);
    ApplyMessageBanners(ILocalNotifier::Instance(), notifications);
  }

  const ShellHost::ChromePrefs chrome = ShellHost::ProjectChrome(prefs);
  if (!last_chrome_ || chrome != *last_chrome_) {
    std::optional<ShellHost::ChromePrefs> previous = last_chrome_;
    last_chrome_ = chrome;
    // SaveProfilePrefs may run on a worker (agent settings tools). Theme / shell chrome
    // touch RmlUi and must apply on the UI thread.
    AppRuntime::PostUI([this, next = chrome, previous = std::move(previous)]() {
      ApplyChrome(next, previous ? &*previous : nullptr);
    });
  }

  // Release 1 ships in English only (temporary, Hongwei 2026-11-25): a changed preference does not change the UI's
  // language (Application::InitializeUiHost applies "en"). To bring the choice back: `.language = prefs.language`.
  const LocalizationService::Prefs locale{.language = "en"};
  if (!last_locale_ || locale != *last_locale_) {
    last_locale_ = locale;
    // SaveProfilePrefs may run on a worker (agent settings tools). Locale listeners
    // remount chrome / touch RmlUi and must run on the UI thread.
    AppRuntime::PostUI([locale]() { LocalizationService::Instance().Apply(locale); });
  }
}

void ConfigApplyBridge::ApplyChrome(const ShellHost::ChromePrefs& next, const ShellHost::ChromePrefs* previous) {
  const bool theme_changed = !previous || next.theme != previous->theme;
  const bool appearance_changed = !previous || next.appearance != previous->appearance;
  if (theme_changed && resolve_asset_) {
    Theme::LoadBase(resolve_asset_(next.theme));
    if (auto* ctx = ui::GetContext("main")) {
      if (ctx->GetNumDocuments() > 0) {
        ctx->GetDocument(0)->UpdateDocument();
      }
    }
  }
  if (appearance_changed) {
    if (auto* ctx = ui::GetContext("main")) {
      Theme::ApplyAppearance(ctx, Theme::ParseAppearance(next.appearance));
    }
  }
  if (ShellHost::NeedsApply(next, previous) && shell_) {
    shell_->Apply(next);
  }
}

} // namespace pbr
