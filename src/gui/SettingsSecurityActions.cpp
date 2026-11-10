#include "gui/SettingsSecurityActions.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/crypto/PinRules.h"
#include "foundation/error/AppError.h"
#include "gui/DiagnosticsFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/DeploymentProfile.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/product/DevTools.h"
#include "foundation/runtime/AppLifecycle.h"
#include "foundation/platform/os/OsTime.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/UserFeedback.h"
#include "lib/ui/DataEvent.h"

#include <ui/base/SystemInterface.h>
#include <ui/data/DataModelHandle.h>

#include <chrono>
#include <ctime>
#include <memory>
#include <optional>
#include <utility>

namespace pbr {

SettingsSecurityActions::SettingsSecurityActions(SettingsHost& host, const SettingsCommands& commands,
                                                 const ShellFeedbackPorts& feedback, const UnlockEnsurePorts& unlock,
                                                 const PinPromptPorts& pin_prompt)
    : host_(host), commands_(commands), feedback_(feedback), unlock_(unlock), pin_prompt_(pin_prompt) {
  // The phrase does not stay up behind the app (the app switcher, an unlocked phone left on the table), and a reveal
  // still being checked is void (second review 2026-11-06).
  background_listener_ = AppLifecycle::AddBackgroundListener([this]() {
    if (!recovery_phrase_.empty()) {
      HideRecoveryPhrase();
    } else {
      ++recovery_shown_count_;
    }
  });
}

SettingsSecurityActions::~SettingsSecurityActions() {
  AppLifecycle::RemoveListener(background_listener_);
}

void SettingsSecurityActions::Bind(ui::DataModelConstructor& ctor) {
  ctor.BindEventCallback("change_pin", DataEvent<>([this]() { ChangePin(); }));
  ctor.BindEventCallback("export_link_device", DataEvent<>([this]() { ExportLinkDevice(); }));
  ctor.BindEventCallback("toggle_biometric", DataEvent<>([this]() { ToggleBiometric(); }));
  ctor.Bind("recovery_phrase", &recovery_phrase_);
  ctor.BindEventCallback("show_recovery_phrase", DataEvent<>([this]() { ShowRecoveryPhrase(); }));
  ctor.BindEventCallback("hide_recovery_phrase", DataEvent<>([this]() { HideRecoveryPhrase(); }));
  ctor.BindEventCallback("save_account_copy", DataEvent<>([this]() { SaveAccountCopy(); }));
  ctor.BindEventCallback("clear_undelivered_older_than", DataEvent<>([this]() { ClearUndeliveredOlderThan(); }));
  ctor.BindEventCallback("reset_tool_permissions", DataEvent<>([this]() { ResetToolPermissions(); }));
  ctor.BindEventCallback("reset_profile", DataEvent<>([this]() { ResetProfile(); }));
  ctor.BindEventCallback("reset_chats", DataEvent<>([this]() { ResetChats(); }));
  show_change_pin_ = ChangePinVisible(kUiDemoBuild);
  ctor.Bind("show_change_pin", &show_change_pin_);
  show_diagnostics_send_ = UploadAllowed(kUiDemoBuild);
  ctor.Bind("show_diagnostics_send", &show_diagnostics_send_);
  ctor.Bind("diagnostics_sending", &diagnostics_sending_);
  ctor.Bind("diagnostics_send_label", &diagnostics_send_label_);
  ctor.Bind("diagnostics_last_id", &diagnostics_last_id_);
  ctor.Bind("diagnostics_last_label", &diagnostics_last_label_);
  ctor.BindEventCallback("send_diagnostics", DataEvent<>([this]() { SendDiagnostics(); }));
  ctor.BindEventCallback("copy_diagnostics_id", DataEvent<>([this]() { CopyDiagnosticsId(); }));
  ctor.BindEventCallback("open_diagnostics_help", DataEvent<>([this]() { OpenDiagnosticsHelp(); }));
}

void SettingsSecurityActions::RefreshDiagnostics() {
  const std::string last = commands_.last_diagnostics_id ? commands_.last_diagnostics_id() : std::string();
  diagnostics_send_label_ =
      Tr(diagnostics_sending_ ? "settings.security.diagnostics.sending" : "settings.security.diagnostics.send").c_str();
  diagnostics_last_id_ = last.c_str();
  diagnostics_last_label_ = last.empty() ? "" : Tr("settings.security.diagnostics.last_id", {{"id", last}}).c_str();
  DataModelHost& model = DataModelHost::Instance();
  model.Dirty("settings", "diagnostics_sending");
  model.Dirty("settings", "diagnostics_send_label");
  model.Dirty("settings", "diagnostics_last_id");
  model.Dirty("settings", "diagnostics_last_label");
}

void SettingsSecurityActions::SendDiagnostics() {
  if (diagnostics_sending_) {
    return;
  }
  if (!commands_.send_diagnostics || !feedback_.show_confirm) {
    host_.ReportFailure(
        Error("Diagnostics are not available").WithUser(Tr("settings.security.diagnostics.unavailable")));
    return;
  }
  feedback_.show_confirm(
      Tr("settings.security.diagnostics.confirm_title"), Tr("settings.security.diagnostics.confirm_body"),
      [this](const bool ok) {
        if (!ok || diagnostics_sending_) {
          return;
        }
        diagnostics_sending_ = true;
        RefreshDiagnostics();
        commands_.send_diagnostics([this](DiagnosticsUploadResult result) {
          diagnostics_sending_ = false;
          const std::string last = commands_.last_diagnostics_id ? commands_.last_diagnostics_id() : std::string();
          // A fresh send has just written its own reference; QuotaKey names the one on file from before.
          const std::string text = DiagnosticsResultText(result, last);
          RefreshDiagnostics();
          if (result.kind == DiagnosticsUploadKind::Sent) {
            host_.ShowStatus("");
            UserFeedback::Ok(text);
          } else {
            host_.ReportFailure(Error(text).WithUser(text));
          }
        });
      },
      Tr("settings.security.diagnostics.confirm_ok"));
}

void SettingsSecurityActions::CopyDiagnosticsId() {
  const std::string id = diagnostics_last_id_.c_str();
  if (id.empty()) {
    return;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(id.c_str());
  }
}

void SettingsSecurityActions::OpenDiagnosticsHelp() {
  // help_key "troubleshooting/send-diagnostics" is the support article; the question goes out at once (Kenneth
  // 2026-11-04: no typing), the answer comes in the PP 客服 chat.
  if (!commands_.ask_support) {
    host_.ReportFailure(Error("Support is not available").WithUser(Tr("settings.support.unavailable")));
    return;
  }
  if (auto asked = commands_.ask_support(Tr("settings.security.diagnostics.help_question")); !asked) {
    host_.ReportFailure(asked.error());
  }
}

void SettingsSecurityActions::EnsureUnlocked(const std::string& not_ready_message, std::function<void()> then) {
  const PinProtectionView pin_state =
      commands_.load_pin_protection ? commands_.load_pin_protection() : PinProtectionView{};
  if (!pin_state.ready) {
    host_.ReportFailure(
        AppError::Pin(Err::Pin::VaultUnavailable, "Set up key protection first").WithUser(not_ready_message));
    return;
  }
  if (pin_state.unlocked) {
    then();
    return;
  }
  auto locked = [this]() { host_.ReportFailure(AppError::Pin(Err::Pin::Required, "Unlock profile PIN first")); };
  if (!unlock_.ensure_unlocked) {
    locked();
    return;
  }
  unlock_.ensure_unlocked([locked, then = std::move(then)](const bool unlocked) { unlocked ? then() : locked(); });
}

void SettingsSecurityActions::CheckPin(const SecretText& pin, std::function<void(Roe<void>)> done) {
  AppRuntime::PostWorkerNormal([this, pin, done = std::move(done)]() {
    // The PIN must open the vault again (not the recovery phrase: linked and older accounts have none, #113). Only
    // pass / fail goes back.
    Roe<void> checked = commands_.verify_pin ? commands_.verify_pin(pin) : Roe<void>(Error("Messaging is not ready"));
    AppRuntime::PostUI([done, checked = std::move(checked)]() { done(checked); });
  });
}

void SettingsSecurityActions::ChangePin() {
  EnsureUnlocked(Tr("settings.security.pin_setup_first"), [this]() {
    if (!commands_.change_pin || !commands_.verify_pin || !pin_prompt_.show_change) {
      host_.ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Vault unavailable"));
      return;
    }
    // The gate checks the current PIN first (a wrong one stays on that step) and compares the new one with its
    // repeat; the engine's change is the same as before. The app default PIN is public: no step asks for it.
    std::optional<SecretText> current_pin;
    if (host_.Store().Snapshot().profile_prefs.pin_is_default) {
      current_pin = SecretText(std::string(kDefaultProfilePin));
    }
    pin_prompt_.show_change(
        [this](const SecretText& pin, std::function<void(Roe<void>)> done) { CheckPin(pin, std::move(done)); },
        [this](const SecretText& old_pin, const SecretText& new_pin, std::function<void(Roe<void>)> done) {
          if (auto changed = commands_.change_pin(old_pin, new_pin); !changed) {
            done(changed.error());
            return;
          }
          ProfilePreferences prefs = host_.Store().Snapshot().profile_prefs;
          prefs.pin_is_default = false;
          prefs.pin_digits = PinDigitsOf(new_pin.Reveal()); // the gate's pad follows the new PIN
          auto saved = host_.Store().SaveProfilePrefs(prefs);
          done({}); // the PIN is changed: the gate closes either way
          if (!saved) {
            host_.ReportFailure(saved.error());
            return;
          }
          host_.Resync(true);
          host_.ShowStatus(Tr("settings.security.pin_updated"));
          UserFeedback::Ok(Tr("settings.security.pin_updated"));
        },
        std::move(current_pin));
  });
}

void SettingsSecurityActions::ToggleBiometric() {
  host_.PullFromView();
  SettingsUiState& state = host_.UiState();
  if (!state.security_biometric_shown) {
    return;
  }
  if (state.security_biometric_on) {
    if (commands_.disable_biometric) {
      commands_.disable_biometric();
    }
    state.security_biometric_on = commands_.biometric_enabled && commands_.biometric_enabled();
    host_.PushToView(false);
    return;
  }
  if (!commands_.enable_biometric || !pin_prompt_.show_verify) {
    host_.ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Face / fingerprint unlock is not available"));
    return;
  }
  pin_prompt_.show_verify(
      PinVerifyPurpose::EnableBiometric,
      [this](const SecretText& pin, std::function<void(Roe<void>)> done) {
        AppRuntime::PostWorkerNormal([this, pin, done = std::move(done)]() {
          // A wrong PIN comes back as the vault's refusal: the gate says so and stays.
          Roe<void> enabled = commands_.enable_biometric
                                  ? commands_.enable_biometric(pin)
                                  : Roe<void>(Error("Face / fingerprint unlock is not available"));
          AppRuntime::PostUI([done, enabled = std::move(enabled)]() { done(enabled); });
        });
      },
      [this]() {
        host_.PullFromView();
        // What the engine holds now, not what was asked for.
        const bool on = commands_.biometric_enabled && commands_.biometric_enabled();
        host_.UiState().security_biometric_on = on;
        host_.PushToView(false);
        if (on) {
          UserFeedback::Ok(Tr("settings.security.biometric.enabled"));
        }
      });
}

void SettingsSecurityActions::ExportLinkDevice() {
  EnsureUnlocked(Tr("settings.security.link_device.not_ready"), [this]() {
    if (!commands_.export_link_device) {
      host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.link_device.not_ready")));
      return;
    }
    // The app's default PIN is public, so asking for it proves nothing: those profiles only confirm, as before.
    if (host_.Store().Snapshot().profile_prefs.pin_is_default) {
      ConfirmExportLinkDevice();
      return;
    }
    // An own PIN (plan #21 ⑧): holding the unlocked phone is not enough to carry the account away.
    if (!commands_.verify_pin || !pin_prompt_.show_verify) {
      host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.link_device.not_ready")));
      return;
    }
    pin_prompt_.show_verify(
        PinVerifyPurpose::ExportTransferCode,
        [this](const SecretText& pin, std::function<void(Roe<void>)> done) { CheckPin(pin, std::move(done)); },
        [this]() { ConfirmExportLinkDevice(); });
  });
}

void SettingsSecurityActions::ConfirmExportLinkDevice() {
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(Tr("settings.security.link_device.export_confirm_title"),
                         Tr("settings.security.link_device.export_confirm_message"),
                         [this](const bool ok) {
                           if (!ok) {
                             return;
                           }
                           AppRuntime::PostWorkerNormal([this]() {
                             auto result = commands_.export_link_device
                                               ? commands_.export_link_device()
                                               : Roe<std::string>{Error("Messaging is not ready")};
                             AppRuntime::PostUI([this, result = std::move(result)]() {
                               if (!result) {
                                 host_.ReportFailure(result.error());
                                 return;
                               }
                               if (ui::SystemInterface* system = ui::GetSystemInterface()) {
                                 system->SetClipboardText(result.value().c_str());
                               }
                               UserFeedback::Ok(Tr("settings.security.link_device.export_done"));
                               host_.ShowStatus("");
                             });
                           });
                         },
                         {});
}

void SettingsSecurityActions::ShowRecoveryPhrase() {
  if (!commands_.recovery_phrase) {
    host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.recovery.not_ready")));
    return;
  }
  // The app's default PIN is public, so it never reveals the phrase: the user sets their own PIN first.
  const bool default_pin = host_.Store().Snapshot().profile_prefs.pin_is_default;
  if (default_pin) {
    UserFeedback::Ok(Tr("settings.security.recovery.set_pin_first"));
    host_.OpenChangePin();
    return;
  }
  if (!pin_prompt_.show_verify) {
    host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.recovery.not_ready")));
    return;
  }
  // The background, leaving the page or a hide while the PIN is being checked voids this reveal (the count moves on).
  const uint64_t request = ++recovery_shown_count_;
  auto revealed = std::make_shared<std::optional<SecretText>>();
  pin_prompt_.show_verify(
      PinVerifyPurpose::ShowRecoveryPhrase,
      [this, revealed](const SecretText& pin, std::function<void(Roe<void>)> done) {
        AppRuntime::PostWorkerNormal([this, pin, revealed, done = std::move(done)]() {
          auto phrase = commands_.recovery_phrase ? commands_.recovery_phrase(pin)
                                                  : Roe<std::optional<SecretText>>(Error("Messaging is not ready"));
          AppRuntime::PostUI([revealed, done, phrase = std::move(phrase)]() {
            if (!phrase) {
              done(phrase.error()); // a wrong PIN: the gate says so and stays
              return;
            }
            *revealed = *phrase;
            done({});
          });
        });
      },
      [this, revealed, request]() {
        if (request != recovery_shown_count_) {
          return;
        }
        if (!*revealed) {
          host_.ReportFailure(Error("No recovery phrase").WithUser(Tr("settings.security.recovery.none")));
          return;
        }
        recovery_phrase_ = (*revealed)->Reveal().c_str(); // shown to its owner, on their screen
        revealed->reset();
        host_.PushToView(false);
        // Home's Security widget: the phrase has been in front of its owner once (Kenneth 2026-11-05 原话 98).
        if (!host_.Store().Snapshot().profile_prefs.recovery_phrase_seen) {
          ProfilePreferences prefs = host_.Store().Snapshot().profile_prefs;
          prefs.recovery_phrase_seen = true;
          (void)host_.Store().SaveProfilePrefs(prefs);
        }
        // Not left on screen: it hides by itself.
        const uint64_t shown = ++recovery_shown_count_;
        AppRuntime::ScheduleCoordinatorOneShot(std::chrono::milliseconds(kRecoveryPhraseShownMs), [this, shown]() {
          AppRuntime::PostUI([this, shown]() {
            if (shown == recovery_shown_count_) {
              HideRecoveryPhrase();
            }
          });
        });
      });
}

void SettingsSecurityActions::HideRecoveryPhrase() {
  recovery_phrase_ = "";
  ++recovery_shown_count_;
  host_.PushToView(false);
}

namespace {

/** `pp-account-2026-10-30.json`: the day it was saved, so copies sort and the newest is easy to tell. */
std::string AccountCopyFileName() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  (void)os::LocalTime(now, &local);
  char day[16] = {};
  std::strftime(day, sizeof(day), "%Y-%m-%d", &local);
  return std::string("pp-account-") + day + ".json";
}

} // namespace

void SettingsSecurityActions::SaveAccountCopy() {
  EnsureUnlocked(Tr("settings.security.copy.not_ready"), [this]() {
    AppRuntime::PostWorkerNormal([this]() {
      auto copy = commands_.export_account_copy ? commands_.export_account_copy()
                                                : Roe<std::string>(Error("Messaging is not ready"));
      AppRuntime::PostUI([this, copy = std::move(copy)]() mutable {
        if (!copy) {
          host_.ReportFailure(copy.error());
          return;
        }
        SaveFileAs(Backend::GetWindow(), AccountCopyFileName(), std::move(*copy), [](const bool saved) {
          AppRuntime::PostUI([saved]() {
            if (saved) {
              UserFeedback::Ok(Tr("settings.security.copy.saved"));
            }
          });
        });
      });
    });
  });
}

void SettingsSecurityActions::ClearUndeliveredOlderThan() {
  if (!commands_.clear_undelivered_older_than) {
    host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.clear_undelivered.not_ready")));
    return;
  }
  if (!feedback_.show_confirm_destructive) {
    return;
  }
  feedback_.show_confirm_destructive(Tr("settings.security.clear_undelivered_confirm_title"),
                                     Tr("settings.security.clear_undelivered_confirm_message"),
                                     [this](const bool ok) {
                                       if (!ok) {
                                         return;
                                       }
                                       AppRuntime::PostWorkerNormal([this]() {
                                         auto result = commands_.clear_undelivered_older_than
                                                           ? commands_.clear_undelivered_older_than(7)
                                                           : Roe<void>{Error("Messaging is not ready")};
                                         AppRuntime::PostUI([this, result = std::move(result)]() {
                                           if (!result) {
                                             host_.ReportFailure(result.error());
                                             return;
                                           }
                                           UserFeedback::Ok(Tr("settings.security.clear_undelivered_done"));
                                           host_.ShowStatus("");
                                         });
                                       });
                                     },
                                     {});
}

void SettingsSecurityActions::ResetToolPermissions() {
  if (!feedback_.show_confirm_destructive) {
    return;
  }
  feedback_.show_confirm_destructive(Tr("settings.security.tool_permissions.reset_title"),
                                     Tr("settings.security.tool_permissions.reset_message"),
                                     [this](const bool ok) {
                                       if (!ok) {
                                         return;
                                       }
                                       if (auto reset = host_.ForgetToolPermissions(); !reset) {
                                         host_.ReportFailure(reset.error());
                                       }
                                     },
                                     {});
}

void SettingsSecurityActions::ResetProfile() {
  if (!feedback_.show_confirm_with_checkbox) {
    return;
  }
  feedback_.show_confirm_with_checkbox(Tr("settings.storage.reset_confirm_title"),
                                       Tr("settings.storage.reset_confirm_message"),
                                       Tr("settings.storage.reset_confirm_check"), false,
                                       [this](const bool confirmed, const bool checked) {
                                         if (!confirmed) {
                                           return;
                                         }
                                         if (!checked) {
                                           UserFeedback::Fail(Tr("settings.storage.reset_confirm_check"));
                                           return;
                                         }
                                         PerformResetProfile();
                                       },
                                       {});
}

void SettingsSecurityActions::PerformResetProfile() {
  if (!commands_.reset_active_profile) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile reset is not available"));
    return;
  }
  if (auto reset = commands_.reset_active_profile(); !reset) {
    host_.ReportFailure(reset.error());
    return;
  }
  HideRecoveryPhrase();
  host_.ShowStatus("");
  host_.Resync(true);
  UserFeedback::Ok(Tr("settings.storage.profile_reset"));
  host_.RequestSyncLayout("settings_profile_reset");
}

void SettingsSecurityActions::ResetChats() {
  if (!feedback_.show_confirm_destructive) {
    return;
  }
  feedback_.show_confirm_destructive(
      Tr("settings.storage.reset_chats_confirm_title"), Tr("settings.storage.reset_chats_confirm_message"),
      [this](const bool ok) {
        if (ok) {
          PerformResetChats();
        }
      },
      Tr("settings.storage.reset_chats_confirm_ok"));
}

void SettingsSecurityActions::PerformResetChats() {
  if (!commands_.reset_chat_data) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Chat reset is not available"));
    return;
  }
  if (auto reset = commands_.reset_chat_data(); !reset) {
    host_.ReportFailure(reset.error());
    return;
  }
  host_.Resync(true);
  UserFeedback::Ok(Tr("settings.storage.chats_reset"));
  host_.RequestSyncLayout("settings_chats_reset");
}

} // namespace pbr
