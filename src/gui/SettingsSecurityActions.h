#pragma once

#include "feature/settings/SettingsCommands.h"
#include "gui/PinPromptPorts.h"
#include "gui/SettingsHost.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/shell/ShellFeedbackPorts.h"

#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

/**
 * Me → Security and Storage's destructive actions: change the PIN, copy a link-device bundle, clear old undelivered messages, forget tool permissions, reset the profile
 * or the chats. Each asks first; the PIN-guarded ones unlock first. The PIN itself is asked on the PIN gate
 * (PinPromptPorts: its number pad for a digit PIN, DEV/PLAN-pin-gate-redesign.md B2), never in a field of this page.
 *
 * UI thread.
 */
class SettingsSecurityActions {
public:
  /** Hides the phrase; also voids a reveal still being checked. */
  void HideRecoveryPhrase();
  SettingsSecurityActions(SettingsHost& host, const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                          const UnlockEnsurePorts& unlock, const PinPromptPorts& pin_prompt);
  ~SettingsSecurityActions();
  SettingsSecurityActions(const SettingsSecurityActions&) = delete;
  SettingsSecurityActions& operator=(const SettingsSecurityActions&) = delete;

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The Send diagnostics button's label and the last-sent line, read again (on opening the section). */
  void RefreshDiagnostics();
  /** The recovery phrase is on screen now (shown, not yet hidden). */
  bool RecoveryPhraseShown() const { return !recovery_phrase_.empty(); }
  /**
   * Me → Security → Copy transfer code (the button's event). With an own PIN the PIN comes first, on the PIN gate
   * (plan #21 ⑧); the app default PIN only confirms.
   */
  void ExportLinkDevice();
  /** Me → Security → Change PIN: the PIN gate's three steps (current, new, again), then the engine's change. */
  void ChangePin();

private:
  /** The PIN opens the vault again (on a worker); `done` on the UI thread. */
  void CheckPin(const SecretText& pin, std::function<void(Roe<void>)> done);
  /** The "copy transfer code?" confirm, then the code to the clipboard. */
  void ConfirmExportLinkDevice();
  /** Me → Security → Show recovery phrase: the PIN again (unless the app default), then the 24 words for a while. */
  void ShowRecoveryPhrase();
  /** Me → Security → Save a copy of my account: the sealed account state to a file the user picks (M023). */
  void SaveAccountCopy();
  void ClearUndeliveredOlderThan();
  void ResetToolPermissions();
  void ResetProfile();
  void PerformResetProfile();
  void ResetChats();
  void PerformResetChats();
  void SendDiagnostics();
  void CopyDiagnosticsId();
  void OpenDiagnosticsHelp();
  /** Runs `then` once the vault is open (asks for the PIN when it is not); reports why not otherwise. */
  void EnsureUnlocked(const std::string& not_ready_message, std::function<void()> then);

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  const UnlockEnsurePorts& unlock_;
  const PinPromptPorts& pin_prompt_;

  /** The phrase while it is shown (empty otherwise). */
  ui::String recovery_phrase_;
  /**
   * Bumped on every reveal, hide and trip to the background: a pending auto-hide, or a reveal whose PIN was still
   * being checked, only acts on the showing it was set for.
   */
  uint64_t recovery_shown_count_ = 0;
  /** Hides the recovery phrase when the app goes to the background (UI thread, SDL's event). */
  uint64_t background_listener_ = 0;
  /** How long the phrase stays on screen. */
  static constexpr int64_t kRecoveryPhraseShownMs = 2 * 60 * 1000;

  bool diagnostics_sending_ = false;
  /** Hidden in the UI demo app (it uploads nothing). */
  bool show_diagnostics_send_ = true;
  bool show_change_pin_ = true;
  ui::String diagnostics_send_label_;
  ui::String diagnostics_last_id_;
  ui::String diagnostics_last_label_;
};

} // namespace pbr
