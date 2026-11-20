#pragma once

#include "feature/settings/SettingsCommands.h"
#include "gui/PinPromptPorts.h"
#include "gui/SettingsHost.h"
#include "gui/UnlockEnsurePorts.h"
#include "gui/shell/ShellFeedbackPorts.h"

#include <ui/base/Types.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
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
  /** Me → Security → Show recovery phrase: the PIN again (unless the app default), then the page opens covered. */
  void ShowRecoveryPhrase();
  /** The grid's one tap: all 24 words come into the view at once. */
  void RevealRecoveryWords();
  SettingsSecurityActions(SettingsHost& host, const SettingsCommands& commands, const ShellFeedbackPorts& feedback,
                          const UnlockEnsurePorts& unlock, const PinPromptPorts& pin_prompt);
  ~SettingsSecurityActions();
  SettingsSecurityActions(const SettingsSecurityActions&) = delete;
  SettingsSecurityActions& operator=(const SettingsSecurityActions&) = delete;

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** A reveal still being checked (the PIN is being verified) is void: it opens nothing when it finishes. */
  void VoidPendingReveal() { ++recovery_shown_count_; }
  /** The recovery phrase is on screen now (shown, not yet hidden). */
  bool RecoveryPhraseShown() const { return recovery_shown_; }
  /** Tests: a short auto-hide time, set before the page opens. */
  void SetRecoveryShownMsForTest(const int64_t ms) { recovery_shown_ms_ = ms; }
  /** Tests: the words are held, not bound, until the reveal (the private copy). */
  size_t RecoverySecretCountForTest() const { return recovery_secret_.size(); }
  /** The words bound into the view now, in order (tests; the words are empty while the grid is covered). */
  std::vector<std::string> RecoveryWordsForTest() const {
    std::vector<std::string> words;
    for (const RecoveryWord& word : recovery_words_) {
      words.emplace_back(word.word.c_str());
    }
    return words;
  }
  /**
   * Me → Security → Copy transfer code (the button's event). With an own PIN the PIN comes first, on the PIN gate
   * (plan #21 ⑧); the app default PIN only confirms.
   */
  void ExportLinkDevice();
  /** Phones' transfer page: the "code copied" box shows (set by the success path of ExportLinkDevice only). */
  bool TransferCodeCopied() const { return transfer_code_copied_; }
  /** Clears the "copied" state and re-reads the page's two validity lines (on opening and on leaving the page). */
  void ClearTransferCopied();
  /**
   * Me → Security → Change PIN: the PIN gate's three steps (current, new, again), then the engine's change. On the app
   * default PIN the current-PIN step is skipped (that PIN is public): new, again.
   */
  void ChangePin();
  /** Me → Security → Remove PIN: the current PIN on the gate, then the vault goes back to the app default PIN. */
  void RemovePin();
  /**
   * Me → Security → "Unlock with Face ID" switch (at-rest A014): on asks for the PIN on the gate first (the engine
   * seals the DEK with it, off the UI thread), then "Turned on"; off needs no PIN.
   */
  void ToggleBiometric();

private:
  /** The PIN opens the vault again (on a worker); `done` on the UI thread. */
  void CheckPin(const SecretText& pin, std::function<void(Roe<void>)> done);
  /**
   * Saves the PIN mode and kind for `new_pin`, turns Face / fingerprint unlock off (its sealed DEK is only for a custom
   * PIN) when the PIN became the default, and tells the app.
   */
  Roe<void> RecordPinChanged(const SecretText& new_pin, bool to_default);
  /** The "copy transfer code?" confirm, then the code to the clipboard. */
  void ConfirmExportLinkDevice();
  /** The transfer page's lead and "copied" lines, with the code's validity in minutes (from the code's own TTL). */
  void SetTransferLabels();
  /** The words and the shown flag redrawn. */
  void DirtyRecoveryWords();
  void ClearRecoverySecret();
  /** Zero-fills each bound word, then empties the list. */
  void ClearRecoveryWords();
  /** (Re)starts the auto-hide: a newer showing or a hide voids the one before. */
  void ScheduleRecoveryAutoHide();

  /** Me → Security → Save a copy of my account: the sealed account state to a file the user picks (M023). */
  void SaveAccountCopy();
  void ClearUndeliveredOlderThan();
  void ResetToolPermissions();
  void ResetProfile();
  void PerformResetProfile();
  void ResetChats();
  void PerformResetChats();
  /** Runs `then` once the vault is open (asks for the PIN when it is not); reports why not otherwise. */
  void EnsureUnlocked(const std::string& not_ready_message, std::function<void()> then);

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const ShellFeedbackPorts& feedback_;
  const UnlockEnsurePorts& unlock_;
  const PinPromptPorts& pin_prompt_;

  /** One numbered word of the phrase, as its page's grid shows it. */
  struct RecoveryWord {
    int n = 0;
    ui::String word;
  };
  /** The phrase's words while it is shown (empty otherwise); cleared when the page closes. */
  std::vector<RecoveryWord> recovery_words_;
  /** One slot while the phrase is shown inline (desktop), none otherwise: the grid in the inline block is mounted by it (data-for), and the phone's page grid by `recovery_page_` the other way round, so the words are bound in one place only. */
  struct InlineSlot {
    int n = 1;
  };
  std::vector<InlineSlot> recovery_inline_;
  /** The same for the phone's page. */
  std::vector<InlineSlot> recovery_page_;
  bool recovery_shown_ = false;
  /** The words are in the view (one tap on the covered grid); until then `recovery_words_` holds numbers only. */
  bool recovery_revealed_ = false;
  /** The words, held here (not bound) until the reveal; cleared with the page. */
  std::vector<std::string> recovery_secret_;
  /**
   * Bumped on every reveal, hide and trip to the background: a pending auto-hide, or a reveal whose PIN was still
   * being checked, only acts on the showing it was set for.
   */
  uint64_t recovery_shown_count_ = 0;
  /** Hides the recovery phrase when the app goes to the background (UI thread, SDL's event). */
  uint64_t background_listener_ = 0;
  /** How long the page stays up (covered, or from the reveal). */
  static constexpr int64_t kRecoveryPhraseShownMs = 2 * 60 * 1000;
  static constexpr size_t kRecoveryWordCount = 24;
  int64_t recovery_shown_ms_ = kRecoveryPhraseShownMs;

  bool transfer_code_copied_ = false;
  ui::String move_lead_label_;
  ui::String move_valid_label_;

  bool show_change_pin_ = true;
};

} // namespace pbr
