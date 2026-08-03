#include <stdexcept>
#include "feature/ui/ClientCompatController.h"

#include "base/i18n/LocalizationService.h"
#include "base/net/ClientCompat.h"
#include "base/runtime/AppVersion.h"
#include "base/runtime/AppRuntime.h"
#include "base/platform/PlatformOpenUrl.h"
#include "common/Logger.h"
#include "feature/ui/UserFeedback.h"

#include <chrono>
#include <optional>

namespace pbr {

ClientCompatController::ClientCompatController() {
  redirectLogger("ClientCompat");
}

namespace {

auto& CompatIoLog() {
  // Free IO helper (no Module instance) — named logger matches redirectLogger.
  static auto logger = logging::getLogger("ClientCompat");
  return logger;
}

int64_t NowUnix() {
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct CompatResolveResult {
  std::optional<ClientCompatDocument> document;
  std::string error;
};

CompatResolveResult ResolveDocumentOnIO(IClientCompatClient* client, const std::string& profile_dir) {
  CompatResolveResult out;
  const int64_t now = NowUnix();
  if (auto cached = LoadClientCompatCache(profile_dir); cached && ClientCompatCacheFresh(*cached, now)) {
    out.document = cached->document;
    return out;
  }

  if (!client) {
    if (auto cached = LoadClientCompatCache(profile_dir); cached) {
      out.document = cached->document;
    } else {
      out.error = "client-compat client unavailable";
    }
    return out;
  }

  auto fetched = client->Fetch();
  if (!fetched) {
    out.error = fetched.error().message;
    if (auto cached = LoadClientCompatCache(profile_dir); cached) {
      out.document = cached->document;
    }
    return out;
  }

  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = now;
  entry.document = *fetched;
  if (auto saved = SaveClientCompatCache(profile_dir, entry); !saved) {
    CompatIoLog().warning << "Failed to cache client-compat: " << saved.error().message;
  }
  out.document = std::move(*fetched);
  return out;
}

} // namespace

void ClientCompatController::BindCompatPorts(MessagingCompatPorts ports) {
  compat_ports_ = std::move(ports);
}

void ClientCompatController::BindShellFeedback(ShellFeedbackPorts ports) {
  shell_feedback_ = std::move(ports);
}

void ClientCompatController::CheckAsync() {
  if (!compat_ports_.messaging_initialized || !compat_ports_.messaging_initialized()) {
    return;
  }
  IClientCompatClient* client = compat_ports_.client_compat ? compat_ports_.client_compat() : nullptr;
  const std::string profile_dir = compat_ports_.profile_data_dir ? compat_ports_.profile_data_dir() : std::string{};

  AppRuntime::PostWorkerAndReplyOnUI<CompatResolveResult>(WorkerLane::Normal,
      [client, profile_dir]() { return ResolveDocumentOnIO(client, profile_dir); },
      [this](CompatResolveResult result) {
        if (!result.document) {
          if (!result.error.empty()) {
            log().info << "client-compat unavailable (fail open): " << result.error;
          }
          last_action_ = CompatUiAction::None;
          return;
        }
        ApplyDocument(*result.document);
      });
}

void ClientCompatController::ApplyDocument(const ClientCompatDocument& doc) {
  last_action_ = DecideCompatUiAction(AppVersionString(), doc);
  PresentAction(last_action_, doc);
}

void ClientCompatController::PresentAction(CompatUiAction action, const ClientCompatDocument& doc) {
  upgrade_url_ = ResolvedUpgradeUrl(doc);
  if (action == CompatUiAction::UpdateRequired) {
    ShowUpdateRequired(doc);
    return;
  }
  if (action == CompatUiAction::SoftUpdateAvailable && !soft_banner_shown_) {
    soft_banner_shown_ = true;
    const std::string message =
        doc.message.empty() ? Tr("compat.update_available.message") : doc.message;
    UserFeedback::NeedsSetup(message);
  }
}

void ClientCompatController::ShowUpdateRequired(const ClientCompatDocument& doc) {
  if (force_dialog_shown_ && shell_feedback_.dialog_active && shell_feedback_.dialog_active()) {
    return;
  }
  force_dialog_shown_ = true;
  const std::string title = Tr("compat.update_required.title");
  const std::string message =
      doc.message.empty() ? Tr("compat.update_required.message") : doc.message;
  const std::string url = upgrade_url_;
  if (shell_feedback_.show_alert) {
    shell_feedback_.show_alert(title, message, [url]() { PlatformOpenUrl(url); },
                               Tr("compat.update_required.action"));
  }
}

} // namespace pbr
