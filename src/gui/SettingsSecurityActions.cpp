#include "gui/SettingsSecurityActions.h"

#include "foundation/crypto/PinDefaults.h"
#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/UserFeedback.h"
#include "lib/ui/DataEvent.h"

#include <ui/base/SystemInterface.h>
#include <ui/data/DataModelHandle.h>

#include <chrono>
#include <ctime>
#include <utility>

namespace pbr {

SettingsSecurityActions::SettingsSecurityActions(SettingsHost& host, const SettingsCommands& commands,
                                                 const ShellFeedbackPorts& feedback, const UnlockEnsurePorts& unlock)
    : host_(host), commands_(commands), feedback_(feedback), unlock_(unlock) {}

void SettingsSecurityActions::Bind(ui::DataModelConstructor& ctor) {
  ctor.Bind("pin_change_old", &pin_change_old_);
  ctor.Bind("pin_change_new", &pin_change_new_);
  ctor.Bind("pin_change_confirm", &pin_change_confirm_);
  ctor.BindEventCallback("change_pin", DataEvent<>([this]() { ChangePin(); }));
  ctor.BindEventCallback("export_link_device", DataEvent<>([this]() { ExportLinkDevice(); }));
  ctor.Bind("recovery_phrase", &recovery_phrase_);
  ctor.Bind("recovery_phrase_pin", &recovery_phrase_pin_);
  ctor.BindEventCallback("show_recovery_phrase", DataEvent<>([this]() { ShowRecoveryPhrase(); }));
  ctor.BindEventCallback("hide_recovery_phrase", DataEvent<>([this]() { HideRecoveryPhrase(); }));
  ctor.BindEventCallback("save_account_copy", DataEvent<>([this]() { SaveAccountCopy(); }));
  ctor.BindEventCallback("clear_undelivered_older_than", DataEvent<>([this]() { ClearUndeliveredOlderThan(); }));
  ctor.BindEventCallback("reset_tool_permissions", DataEvent<>([this]() { ResetToolPermissions(); }));
  ctor.BindEventCallback("reset_profile", DataEvent<>([this]() { ResetProfile(); }));
  ctor.BindEventCallback("reset_chats", DataEvent<>([this]() { ResetChats(); }));
}

void SettingsSecurityActions::ClearPinFields() {
  pin_change_old_ = "";
  pin_change_new_ = "";
  pin_change_confirm_ = "";
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
  auto locked = [this]() {
    host_.ReportFailure(AppError::Pin(Err::Pin::Required, "Unlock profile PIN first"));
  };
  if (!unlock_.ensure_unlocked) {
    locked();
    return;
  }
  unlock_.ensure_unlocked([locked, then = std::move(then)](const bool unlocked) {
    unlocked ? then() : locked();
  });
}

void SettingsSecurityActions::ChangePin() {
  EnsureUnlocked(Tr("settings.security.pin_setup_first"), [this]() {
    const std::string old_pin = pin_change_old_.c_str();
    const std::string new_pin = pin_change_new_.c_str();
    const std::string confirm = pin_change_confirm_.c_str();
    if (old_pin.empty() || new_pin.empty()) {
      host_.ReportFailure(AppError::Pin(Err::Pin::Required, "Current and new PIN are required")
                              .WithUser(Tr("settings.security.pin_both_required")));
      return;
    }
    if (new_pin != confirm) {
      host_.ReportFailure(AppError::Pin(Err::Pin::Mismatch, "New PINs do not match"));
      return;
    }
    if (new_pin.size() < 4) {
      host_.ReportFailure(AppError::Pin(Err::Pin::TooShort, "Use at least 4 characters"));
      return;
    }
    if (!commands_.change_pin) {
      host_.ReportFailure(AppError::Pin(Err::Pin::VaultUnavailable, "Vault unavailable"));
      return;
    }
    if (auto changed = commands_.change_pin(old_pin, new_pin); !changed) {
      host_.ReportFailure(changed.error());
      return;
    }
    ProfilePreferences prefs = host_.Store().Snapshot().profile_prefs;
    prefs.pin_is_default = false;
    if (auto saved = host_.Store().SaveProfilePrefs(prefs); !saved) {
      host_.ReportFailure(saved.error());
      return;
    }
    ClearPinFields();
    host_.Resync(true);
    host_.ShowStatus(Tr("settings.security.pin_updated"));
    UserFeedback::Ok(Tr("settings.security.pin_updated"));
  });
}

void SettingsSecurityActions::ExportLinkDevice() {
  EnsureUnlocked(Tr("settings.security.link_device.not_ready"), [this]() {
    if (!commands_.export_link_device) {
      host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.link_device.not_ready")));
      return;
    }
    if (!feedback_.show_confirm) {
      return;
    }
    feedback_.show_confirm(
        Tr("settings.security.link_device.export_confirm_title"),
        Tr("settings.security.link_device.export_confirm_message"),
        [this](const bool ok) {
          if (!ok) {
            return;
          }
          AppRuntime::PostWorkerNormal([this]() {
            auto result = commands_.export_link_device ? commands_.export_link_device()
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
  });
}

void SettingsSecurityActions::ShowRecoveryPhrase() {
  if (!commands_.recovery_phrase) {
    host_.ReportFailure(Error("Messaging is not ready").WithUser(Tr("settings.security.recovery.not_ready")));
    return;
  }
  // A profile on the app default PIN gets its phrase as it gets everything else (by design: the unlocked app is
  // enough there); a chosen PIN is asked for again.
  const bool default_pin = host_.Store().Snapshot().profile_prefs.pin_is_default;
  const std::string pin = default_pin ? std::string(kDefaultProfilePin) : std::string(recovery_phrase_pin_.c_str());
  if (pin.empty()) {
    host_.ReportFailure(AppError::Pin(Err::Pin::Required, "PIN required").WithUser(Tr("settings.security.recovery.pin_required")));
    return;
  }
  recovery_phrase_pin_ = "";
  AppRuntime::PostWorkerNormal([this, pin]() {
    auto phrase = commands_.recovery_phrase ? commands_.recovery_phrase(pin)
                                            : Roe<std::optional<std::string>>(Error("Messaging is not ready"));
    AppRuntime::PostUI([this, phrase = std::move(phrase)]() mutable {
      if (!phrase) {
        host_.ReportFailure(phrase.error());
        host_.PushToView(false);
        return;
      }
      if (!*phrase) {
        host_.ReportFailure(Error("No recovery phrase").WithUser(Tr("settings.security.recovery.none")));
        return;
      }
      recovery_phrase_ = phrase->value().c_str();
      host_.PushToView(false);
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
  });
}

void SettingsSecurityActions::HideRecoveryPhrase() {
  recovery_phrase_ = "";
  recovery_phrase_pin_ = "";
  ++recovery_shown_count_;
  host_.PushToView(false);
}

namespace {

/** `pp-account-2026-10-30.json`: the day it was saved, so copies sort and the newest is easy to tell. */
std::string AccountCopyFileName() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#if defined(_WIN32)
  localtime_s(&local, &now);
#else
  localtime_r(&now, &local);
#endif
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
    host_.ReportFailure(
        Error("Messaging is not ready").WithUser(Tr("settings.security.clear_undelivered.not_ready")));
    return;
  }
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(
      Tr("settings.security.clear_undelivered_confirm_title"),
      Tr("settings.security.clear_undelivered_confirm_message"),
      [this](const bool ok) {
        if (!ok) {
          return;
        }
        AppRuntime::PostWorkerNormal([this]() {
          auto result = commands_.clear_undelivered_older_than ? commands_.clear_undelivered_older_than(7)
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
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(Tr("settings.security.tool_permissions.reset_title"),
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
  feedback_.show_confirm_with_checkbox(
      Tr("settings.storage.reset_confirm_title"), Tr("settings.storage.reset_confirm_message"),
      Tr("settings.storage.reset_confirm_check"), false, [this](const bool confirmed, const bool checked) {
        if (!confirmed) {
          return;
        }
        if (!checked) {
          UserFeedback::Fail(Tr("settings.storage.reset_confirm_check"));
          return;
        }
        PerformResetProfile();
      });
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
  ClearPinFields();
  host_.ShowStatus("");
  host_.Resync(true);
  UserFeedback::Ok(Tr("settings.storage.profile_reset"));
  host_.RequestSyncLayout("settings_profile_reset");
}

void SettingsSecurityActions::ResetChats() {
  if (!feedback_.show_confirm) {
    return;
  }
  feedback_.show_confirm(Tr("settings.storage.reset_chats_confirm_title"),
                         Tr("settings.storage.reset_chats_confirm_message"),
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
