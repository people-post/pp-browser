#pragma once

#include "lib/net/ClientCompat.h"
#include "common/Module.h"
#include "feature/conversations/MessagingCompatPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/SupportDiscoveryPorts.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Fetches relay client-compat and surfaces update-required / update-available UX (docs/contracts/SERVICE_ENDPOINTS.md
 * § Client compatibility). Checked at start-up (a fresh cache as it is), again when the app comes to the foreground
 * (forced, at most once per kClientCompatRecheckSeconds since the last fetch), and when the org's relay answers `426
 * client_upgrade_required` (forced; the update screen shows whatever the document says). UI thread.
 */
class ClientCompatController : public Module {
public:
  ClientCompatController();

  /** Subscribes to the org relay's 426 through `ports` (the previous ports' subscription goes). */
  void BindCompatPorts(MessagingCompatPorts ports);
  /** Dialog / banner feedback. Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Publish app Support discovery after each resolved document. Clear via BindSupportDiscovery({}). */
  void BindSupportDiscovery(SupportDiscoveryPorts ports);
  /** Tests: the clock (Unix seconds). */
  void SetClock(std::function<int64_t()> now_unix);

  /**
   * Non-blocking: IO fetch/cache then UI gate/banner. Safe after ConversationsHub init. Forced: asks the relay even
   * with a fresh cache (a failed fetch still falls back to it). False when no check started (messaging not ready, or
   * one in flight: its answer applies).
   */
  bool CheckAsync(CompatCheck mode = CompatCheck::CacheFirst);
  /** The app came to the foreground: a forced check once the document held is kClientCompatRecheckSeconds old. */
  void OnForeground();
  /**
   * The org's relay refused a call `426 client_upgrade_required`: the update screen (with the document's links: a
   * forced check first unless one ran a moment ago), for the rest of the session, whatever the document's floor.
   */
  void OnOrgRelayRequiresUpgrade();

  /** Apply a resolved document on the UI thread (also used by tests). */
  void ApplyDocument(const ClientCompatDocument& doc);

  CompatUiAction LastAction() const { return last_action_; }
  /** A check started and its answer is not applied yet. */
  bool CheckInFlight() const { return check_in_flight_; }
  /** The link the update screen opens (ResolvedUpgradeUrl for this platform). */
  const std::string& UpgradeUrl() const { return upgrade_url_; }

private:
  void OnResolved(ClientCompatResolution result);
  void PresentAction(CompatUiAction action, const ClientCompatDocument& doc);
  void ShowUpdateRequired(const ClientCompatDocument& doc);
  void PublishSupport(const ClientCompatDocument& doc);
  int64_t NowUnix() const;

  CompatUiAction last_action_ = CompatUiAction::None;
  bool soft_banner_shown_ = false;
  bool force_dialog_shown_ = false;
  bool check_in_flight_ = false;
  /** The org's relay answered 426 this session: the update screen, whatever the document says. */
  bool relay_requires_upgrade_ = false;
  /** When the document held was fetched (Unix seconds; 0: none yet). */
  int64_t fetched_at_unix_ = 0;
  std::optional<ClientCompatDocument> document_;
  std::string upgrade_url_;
  std::function<int64_t()> now_unix_;
  MessagingCompatPorts compat_ports_;
  ShellFeedbackPorts shell_feedback_;
  SupportDiscoveryPorts support_discovery_;
};

} // namespace pbr
