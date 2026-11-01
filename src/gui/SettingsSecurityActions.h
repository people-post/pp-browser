#pragma once

#include "feature/settings/SettingsCommands.h"
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
 * Me → Security and Storage's destructive actions: change the PIN (its three fields are this part's),
 * copy a link-device bundle, clear old undelivered messages, forget tool permissions, reset the profile
 * or the chats. Each asks first; the PIN-guarded ones unlock first.
 *
 * UI thread.
 */
class SettingsSecurityActions {
public:
  SettingsSecurityActions(SettingsHost& host, const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                          const UnlockEnsurePorts& unlock);

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The Send diagnostics button's label and the last-sent line, read again (on opening the section). */
  void RefreshDiagnostics();

private:
  void ChangePin();
  void ExportLinkDevice();
  /** Me → Security → Show recovery phrase: the PIN again (unless the app default), then the 24 words for a while. */
  void ShowRecoveryPhrase();
  void HideRecoveryPhrase();
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
  void ClearPinFields();

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  const UnlockEnsurePorts& unlock_;

  ui::String pin_change_old_;
  ui::String pin_change_new_;
  ui::String pin_change_confirm_;
  /** The phrase while it is shown (empty otherwise) and the PIN typed to show it. */
  ui::String recovery_phrase_;
  ui::String recovery_phrase_pin_;
  /** Bumped on every show and hide: a pending auto-hide only acts on the showing it was set for. */
  uint64_t recovery_shown_count_ = 0;
  /** How long the phrase stays on screen. */
  static constexpr int64_t kRecoveryPhraseShownMs = 2 * 60 * 1000;

  bool diagnostics_sending_ = false;
  ui::String diagnostics_send_label_;
  ui::String diagnostics_last_id_;
  ui::String diagnostics_last_label_;
};

} // namespace pbr
