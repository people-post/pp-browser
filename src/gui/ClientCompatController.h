#pragma once

#include "lib/net/ClientCompat.h"
#include "common/Module.h"
#include "feature/conversations/MessagingCompatPorts.h"
#include "gui/shell/ShellFeedbackPorts.h"
#include "gui/SupportDiscoveryPorts.h"
#include "gui/UpdateCard.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Fetches relay client-compat and surfaces update-required / update-available UX (docs/contracts/SERVICE_ENDPOINTS.md
 * § Client compatibility). Checked at start-up (a fresh cache as it is), again when the app comes to the foreground
 * (forced, at most once per kClientCompatRecheckSeconds since the last fetch), while it runs (forced, once the
 * document held is kClientCompatTimerRecheckSeconds old; T15), and when the org's relay answers `426
 * client_upgrade_required` (forced, for the document's links). What it shows (RC018 part 2 default, T14): below the
 * document's floor the full screen (the blocking dialog); while the org's relay refuses this app, the Chats page's
 * update card held up (no "Later", no alert: a refusal at every poll raises nothing new); a newer version, the card.
 * The first document a run resolves gives the run its remote switches (RemoteSwitches, O2). UI thread.
 */
class ClientCompatController : public Module {
public:
  ClientCompatController();
  ~ClientCompatController() override;

  /** Subscribes to the org relay's 426 through `ports` (the previous ports' subscription goes). */
  void BindCompatPorts(MessagingCompatPorts ports);
  /** Dialog / banner feedback. Clear via BindShellFeedback({}). */
  void BindShellFeedback(ShellFeedbackPorts ports);
  /** Publish app Support discovery after each resolved document. Clear via BindSupportDiscovery({}). */
  void BindSupportDiscovery(SupportDiscoveryPorts ports);
  /** The "new version" card's source changed (UI thread); read it with SoftUpdate(). Clear via {}. */
  void BindSoftUpdateChanged(std::function<void()> changed) { on_soft_update_changed_ = std::move(changed); }
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
   * The running app's timer (every kRecheckTimerMs, armed by the first check): a forced check once the document held
   * is kClientCompatTimerRecheckSeconds old (T15: a floor raised while the app runs is seen within hours).
   */
  void OnRecheckTimer();
  static constexpr int64_t kRecheckTimerMs = 60 * 60 * 1000;
  /**
   * The org's relay refused a call `426 client_upgrade_required`: the update card held up for the rest of the session
   * (with the document's links: a forced check first unless one ran a moment ago), whatever the document's floor. The
   * full screen stays the document's floor's alone; a refusal again changes nothing on screen.
   */
  void OnOrgRelayRequiresUpgrade();

  /** Apply a resolved document on the UI thread (also used by tests). */
  void ApplyDocument(const ClientCompatDocument& doc);

  CompatUiAction LastAction() const { return last_action_; }
  /** A check started and its answer is not applied yet. */
  bool CheckInFlight() const { return check_in_flight_; }
  /**
   * What the Chats page's card shows: a newer version than this one exists (the document's latest_client_version), or
   * the org's relay refuses this app (UpdateAdvice::held). Nothing when up to date, when the full screen is the
   * answer (below the floor), or before any document and refusal.
   */
  const std::optional<UpdateAdvice>& SoftUpdate() const { return soft_update_; }
  /** The link the update screen opens (ResolvedUpgradeUrl for this platform). */
  const std::string& UpgradeUrl() const { return upgrade_url_; }

private:
  void OnResolved(ClientCompatResolution result);
  void PresentAction(CompatUiAction action, const ClientCompatDocument& doc);
  void ShowUpdateRequired(const ClientCompatDocument& doc);
  void PublishSupport(const ClientCompatDocument& doc);
  void SetSoftUpdate(std::optional<UpdateAdvice> advice);
  int64_t NowUnix() const;
  void EnsureRecheckTimer();
  void StopRecheckTimer();

  CompatUiAction last_action_ = CompatUiAction::None;
  bool force_dialog_shown_ = false;
  bool check_in_flight_ = false;
  /** The org's relay answered 426 this session: the update screen, whatever the document says. */
  bool relay_requires_upgrade_ = false;
  /** When the document held was fetched (Unix seconds; 0: none yet). */
  int64_t fetched_at_unix_ = 0;
  std::optional<ClientCompatDocument> document_;
  std::string upgrade_url_;
  std::optional<UpdateAdvice> soft_update_;
  std::function<void()> on_soft_update_changed_;
  std::function<int64_t()> now_unix_;
  uint64_t recheck_timer_id_ = 0;
  /** The timer's tasks hold this weakly: one that fires after the controller is gone does nothing. */
  std::shared_ptr<ClientCompatController*> alive_ = std::make_shared<ClientCompatController*>(this);
  /** The document's floor was not a version (logged once a run). */
  bool floor_unreadable_logged_ = false;
  MessagingCompatPorts compat_ports_;
  ShellFeedbackPorts shell_feedback_;
  SupportDiscoveryPorts support_discovery_;
};

} // namespace pbr
