#include "gui/ClientCompatController.h"

#include "foundation/i18n/LocalizationService.h"
#include "lib/net/ClientCompat.h"
#include "lib/net/RemoteSwitches.h"
#include "foundation/product/AppVersion.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/PlatformOpenUrl.h"
#include "common/Logger.h"
#include "gui/UpdateLink.h"

#include <chrono>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

ClientCompatController::ClientCompatController() {
  redirectLogger("ClientCompat");
}

ClientCompatController::~ClientCompatController() {
  StopRecheckTimer();
}

void ClientCompatController::EnsureRecheckTimer() {
  if (recheck_timer_id_ != 0 || !AppRuntime::IsRunning()) {
    return;
  }
  std::weak_ptr<ClientCompatController*> alive = alive_;
  recheck_timer_id_ = AppRuntime::ScheduleCoordinatorRepeating(std::chrono::milliseconds(kRecheckTimerMs), [alive]() {
    AppRuntime::PostUI([alive]() {
      if (const auto held = alive.lock()) {
        (*held)->OnRecheckTimer();
      }
    });
  });
}

void ClientCompatController::StopRecheckTimer() {
  if (recheck_timer_id_ != 0) {
    AppRuntime::CancelCoordinatorTimer(recheck_timer_id_);
    recheck_timer_id_ = 0;
  }
}

void ClientCompatController::OnRecheckTimer() {
  if (ClientCompatRecheckDue(fetched_at_unix_, NowUnix(), kClientCompatTimerRecheckSeconds)) {
    (void)CheckAsync(CompatCheck::Forced);
  }
}

void ClientCompatController::BindCompatPorts(MessagingCompatPorts ports) {
  if (!ports.messaging_initialized) {
    StopRecheckTimer(); // unbound: nothing to check against (the next bind's first check arms it again)
  }
  if (compat_ports_.watch_upgrade_required) {
    compat_ports_.watch_upgrade_required(nullptr);
  }
  compat_ports_ = std::move(ports);
  if (compat_ports_.watch_upgrade_required) {
    // Any thread (the relay call's): handled on the UI thread.
    compat_ports_.watch_upgrade_required([this]() { AppRuntime::PostUI([this]() { OnOrgRelayRequiresUpgrade(); }); });
  }
}

void ClientCompatController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void ClientCompatController::BindSupportDiscovery(SupportDiscoveryPorts ports) {
  support_discovery_ = std::move(ports);
}

void ClientCompatController::SetClock(std::function<int64_t()> now_unix) {
  now_unix_ = std::move(now_unix);
}

int64_t ClientCompatController::NowUnix() const {
  if (now_unix_) {
    return now_unix_();
  }
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

bool ClientCompatController::CheckAsync(const CompatCheck mode) {
  if (check_in_flight_ || !compat_ports_.messaging_initialized || !compat_ports_.messaging_initialized()) {
    return false;
  }
  std::shared_ptr<IClientCompatClient> client = compat_ports_.client_compat ? compat_ports_.client_compat() : nullptr;
  const std::string profile_dir = compat_ports_.profile_data_dir ? compat_ports_.profile_data_dir() : std::string{};
  check_in_flight_ = true;
  EnsureRecheckTimer();
  AppRuntime::PostWorkerAndReplyOnUI<ClientCompatResolution>(
      WorkerLane::Normal,
      [client, profile_dir, mode, now = NowUnix()]() {
        return ResolveClientCompat(client.get(), profile_dir, now, mode);
      },
      [this](ClientCompatResolution result) { OnResolved(std::move(result)); });
  return true;
}

void ClientCompatController::OnResolved(ClientCompatResolution result) {
  check_in_flight_ = false;
  if (!result.document) {
    if (!result.error.empty()) {
      log().info << "client-compat unavailable (fail open): " << result.error;
    }
    last_action_ = CompatUiAction::None;
    if (relay_requires_upgrade_) {
      PresentAction(CompatUiAction::UpdateRequired, document_.value_or(ClientCompatDocument{}));
    }
    return;
  }
  if (!result.error.empty()) {
    if (result.fetched) {
      log().warning << result.error; // the cache was not written; the document applies
    } else {
      log().info << "client-compat fetch failed, cached copy used: " << result.error;
    }
  }
  fetched_at_unix_ = result.fetched_at_unix;
  ApplyDocument(*result.document);
}

void ClientCompatController::OnForeground() {
  if (ClientCompatRecheckDue(fetched_at_unix_, NowUnix())) {
    (void)CheckAsync(CompatCheck::Forced);
  }
}

void ClientCompatController::OnOrgRelayRequiresUpgrade() {
  const bool first = !relay_requires_upgrade_;
  relay_requires_upgrade_ = true;
  if (check_in_flight_) {
    return; // its answer shows the screen
  }
  if (first || ClientCompatRecheckDue(fetched_at_unix_, NowUnix())) {
    if (first) {
      log().warning << "the org's relay needs a newer version of this app (426 client_upgrade_required)";
    }
    if (CheckAsync(CompatCheck::Forced)) {
      return; // for the document's links and message
    }
  }
  PresentAction(CompatUiAction::UpdateRequired, document_.value_or(ClientCompatDocument{}));
}

void ClientCompatController::ApplyDocument(const ClientCompatDocument& doc) {
  document_ = doc;
  RemoteSwitches::Load(doc.switches); // the run's first document only (O2)
  if (ClientCompatFloorUnreadable(doc) && !floor_unreadable_logged_) {
    floor_unreadable_logged_ = true;
    log().warning << "client-compat min_client_version is not a version; no floor applies: " << doc.min_client_version;
  }
  last_action_ = DecideCompatUiAction(AppVersionString(), doc, relay_requires_upgrade_);
  PresentAction(last_action_, doc);
  PublishSupport(doc);
}

void ClientCompatController::PublishSupport(const ClientCompatDocument& doc) {
  if (!support_discovery_.on_support_changed) {
    return;
  }
  std::optional<ClientCompatSupport> published;
  if (doc.support && doc.support->enabled && !doc.support->account_id.empty()) {
    published = *doc.support;
  }
  support_discovery_.on_support_changed(published);
}

void ClientCompatController::PresentAction(CompatUiAction action, const ClientCompatDocument& doc) {
  upgrade_url_ = ResolvedUpgradeUrl(doc, kDefaultUpgradeUrl, Platform::OsName());
  if (action == CompatUiAction::UpdateRequired) {
    last_action_ = action;
    SetSoftUpdate(std::nullopt); // the dialog says it
    ShowUpdateRequired(doc);
    return;
  }
  if (action == CompatUiAction::SoftUpdateAvailable) {
    SetSoftUpdate(UpdateAdvice{doc.latest_client_version, UsableUpdateLink(doc, Platform::OsName(), kDefaultUpgradeUrl),
                               doc.message});
    return;
  }
  SetSoftUpdate(std::nullopt); // up to date, or the update screen says it
}

void ClientCompatController::SetSoftUpdate(std::optional<UpdateAdvice> advice) {
  const bool same = soft_update_.has_value() == advice.has_value() &&
                    (!advice || (soft_update_->latest_version == advice->latest_version &&
                                 soft_update_->url == advice->url && soft_update_->message == advice->message));
  if (same) {
    return;
  }
  soft_update_ = std::move(advice);
  if (on_soft_update_changed_) {
    on_soft_update_changed_();
  }
}

void ClientCompatController::ShowUpdateRequired(const ClientCompatDocument& doc) {
  if (force_dialog_shown_ && shell_feedback_.dialog_active && shell_feedback_.dialog_active()) {
    return;
  }
  force_dialog_shown_ = true;
  const std::string title = Tr("compat.update_required.title");
  // The app's own words always; the relay's message, when it sent one, is a second paragraph (plain text, capped).
  std::string message = Tr("compat.update_required.message");
  if (const std::string note = UpdateServerNote(doc.message); !note.empty()) {
    message += "\n\n" + note;
  }
  const std::string url = upgrade_url_;
  if (shell_feedback_.show_alert) {
    shell_feedback_.show_alert(title, message, [url]() { PlatformOpenUrl(url); }, Tr("compat.update_required.action"));
  }
}

} // namespace pbr
