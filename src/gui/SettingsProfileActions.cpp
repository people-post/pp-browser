#include "gui/SettingsProfileActions.h"

#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/CameraCapture.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/runtime/AppRuntime.h"
#include "common/Logger.h"
#include "common/PbrCompat.h"
#include "domain/registration/RegistrationClient.h"
#include "gui/AccountInvite.h"
#include "gui/BlobQuotaRecoveryFlow.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/ContextMenuHost.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/QrSvg.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/data/DataModelHandle.h>
#include <SDL3/SDL.h>

#include <memory>
#include <utility>
#include <vector>

namespace pbr {

/** Answers the crop page once: `Answer(ok)`, or `false` when the last holder lets go unanswered (a PIN step that
 *  dropped its closure), so the page can never stay locked. */
struct AvatarCropFinish {
  explicit AvatarCropFinish(AvatarCropReply r) : reply(std::move(r)) {}
  ~AvatarCropFinish() { Answer(false); }
  void Uploading(const bool running) {
    if (!answered && reply.uploading) {
      reply.uploading(running);
    }
  }
  void Answer(const bool ok) {
    if (answered) {
      return;
    }
    answered = true;
    if (reply.uploading) {
      reply.uploading(false);
    }
    if (reply.finished) {
      reply.finished(ok);
    }
  }
  AvatarCropReply reply;
  bool answered = false;
};

SettingsProfileActions::SettingsProfileActions(SettingsHost& host, const SettingsCommands& commands,
                                               const UnlockEnsurePorts& unlock)
    : host_(host), commands_(commands), unlock_(unlock) {}

void SettingsProfileActions::Bind(ui::DataModelConstructor& ctor) {
  ctor.Bind("profile_qr_svg_rml", &qr_svg_rml_);
  ctor.Bind("profile_show_qr", &show_qr_);
  ctor.BindEventCallback("show_my_qr", DataEvent<>([this]() {
                           if (show_my_qr_) {
                             show_my_qr_();
                           }
                         }));
  ctor.BindEventCallback("register_profile", DataEvent<>([this]() { Register(); }));
  ctor.BindEventCallback("rotate_brief_llm_key", DataEvent<>([this]() { RotateBriefLlmKey(); }));
  ctor.BindEventCallback("copy_profile_id", DataEvent<>([this]() { CopyId(); }));
  ctor.BindEventCallback("tap_profile_avatar", DataEventAt([this](ui::Event& ev) { TapAvatar(ev); }));
  ctor.BindEventCallback("pick_profile_icon", DataEventAt([this](ui::Event& ev) { PickIcon(ev); }));
  ctor.BindEventCallback("clear_profile_icon", DataEvent<>([this]() { ClearIcon(); }));
  ctor.BindEventCallback("share_profile", DataEventAt([this](ui::Event& ev) { Share(ShareAnchorFromEvent(ev)); }));
  ctor.BindEventCallback("toggle_profile_qr", DataEvent<>([this]() { ToggleQr(); }));
}

void SettingsProfileActions::ShowInvite(const std::string& locator) {
  if (locator == qr_locator_) {
    return;
  }
  qr_locator_ = locator;
  auto qr = locator.empty() ? Roe<std::string>(std::string()) : QrSvgContent(locator);
  qr_svg_rml_ = qr ? qr.value().c_str() : "";
  DataModelHost::Instance().Dirty("settings", "profile_qr_svg_rml");
}

void SettingsProfileActions::ToggleQr() {
  show_qr_ = !show_qr_;
  DataModelHost::Instance().Dirty("settings", "profile_show_qr");
}

void SettingsProfileActions::Unlocked(const char* locked, std::function<void()> then,
                                      std::function<void()> not_unlocked) {
  if (!unlock_.ensure_unlocked) {
    host_.ReportFailure(AppError::Pin(Err::Pin::Required, locked));
    if (not_unlocked) {
      not_unlocked();
    }
    return;
  }
  unlock_.ensure_unlocked(
      [this, locked, then = std::move(then), not_unlocked = std::move(not_unlocked)](const bool unlocked) {
        if (!unlocked) {
          host_.ReportFailure(AppError::Pin(Err::Pin::Required, locked));
          if (not_unlocked) {
            not_unlocked();
          }
          return;
        }
        then();
      });
}

namespace {

/** A reconnect that failed (B216), in words for its cause: the server refused it, or the network did not answer. */
Error ReconnectFailure(Error error) {
  if (AppError::CategoryOf(error) != ErrorCategory::Network) {
    return error; // the PIN and the rest keep their own words
  }
  const bool refused = IsRegistrationRefused(error);
  return std::move(error).WithUser(refused ? Tr("chat.reconnect.refused") : Tr("chat.reconnect.offline"));
}

} // namespace

void SettingsProfileActions::BindRunWhenRegistrationIdle(std::function<void(std::function<void()>)> run) {
  run_when_registration_idle_ = std::move(run);
}

void SettingsProfileActions::BindShowMyQr(std::function<void()> show) {
  show_my_qr_ = std::move(show);
}

void SettingsProfileActions::Register(const bool quiet_success, std::function<void(bool ok)> done) {
  // The reconnect (B216) never reads the settings page: it may never have been opened in this run, and what its
  // fields hold is not the registration's to commit or send.
  if (!quiet_success) {
    host_.SaveProfileNickname();
    host_.PullFromView();
  }
  const bool renewing = !quiet_success && host_.UiState().profile_registered == "yes";
  auto finished = std::make_shared<std::function<void(bool)>>(std::move(done));
  Unlocked(
      "PIN required to register",
      [this, renewing, quiet_success, finished]() {
        // Never beside the automatic renewal (the one unlocking just started included): after it, when one is out.
        auto run = [this, renewing, quiet_success, finished]() {
          const bool registered = RegisterUnlocked(renewing, quiet_success);
          if (*finished) {
            (*finished)(registered);
          }
        };
        if (run_when_registration_idle_) {
          run_when_registration_idle_(std::move(run));
        } else {
          run();
        }
      },
      [finished]() {
        if (*finished) {
          (*finished)(false); // the PIN was not given
        }
      });
}

bool SettingsProfileActions::RegisterUnlocked(const bool renewing, const bool quiet_success) {
  if (!commands_.register_identity) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Registration is not available"));
    return false;
  }
  // Reconnect: the stored nickname (as the automatic renewal does); "" keeps the stored one in the hub.
  std::string nickname = host_.UiState().profile_nickname;
  if (quiet_success) {
    nickname.clear();
    if (commands_.load_profile_identity) {
      if (const ProfileIdentityView view = commands_.load_profile_identity(); view.ready) {
        nickname = view.nickname;
      }
    }
  }
  if (auto registered = commands_.register_identity({.nickname = nickname}); !registered) {
    host_.ReportFailure(quiet_success ? ReconnectFailure(registered.error()) : registered.error());
    return false;
  }
  if (!quiet_success && commands_.load_profile_identity) {
    const ProfileIdentityView view = commands_.load_profile_identity();
    if (view.ready) {
      UiEditSession::Instance().OnCommitted(kUiFieldProfileNickname, view.nickname);
    }
  }
  host_.Resync();
  if (quiet_success) {
    return true; // "Reconnected" is said when a poll gets through (ChatController)
  }
  const std::string message = renewing ? Tr("settings.registration.renewed") : Tr("settings.registration.done");
  host_.ShowStatus(message);
  UserFeedback::Ok(message);
  return true;
}

void SettingsProfileActions::RotateBriefLlmKey() {
  host_.PullFromView();
  Unlocked("PIN required to rotate API key", [this]() {
    if (!commands_.rotate_brief_llm_key) {
      host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Key rotation is not available"));
      return;
    }
    if (auto rotated = commands_.rotate_brief_llm_key(); !rotated) {
      host_.Resync(); // the identity may have been marked expired (401 / 403)
      host_.ReportFailure(rotated.error());
      return;
    }
    host_.Resync();
    host_.ShowStatus(Tr("settings.llm.brief_key_rotated"));
    UserFeedback::Ok(Tr("settings.llm.brief_key_rotated"));
  });
}

void SettingsProfileActions::CopyId() {
  host_.PullFromView();
  // The account ID: what the row shows, what Share sends and what a friend types into Add contact.
  const std::string& account_id = host_.UiState().profile_relay_id;
  if (account_id.empty()) {
    UserFeedback::Fail(Tr("settings.profile.no_peer_id"));
    return;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(account_id.c_str());
  }
  UserFeedback::Ok(Tr("settings.profile.peer_id_copied"));
}

void SettingsProfileActions::TapAvatar(ui::Event& ev) {
  if (host_.UiState().profile_icon_uploading) {
    return;
  }
  const std::string& icon = host_.UiState().profile_icon_src;
  if (PlanProfileAvatarTap(host_.UiState().profile_has_icon && !icon.empty(), Platform::IsMobile()) ==
          ProfileAvatarTapAction::ViewPhoto &&
      commands_.open_avatar_viewer) {
    commands_.open_avatar_viewer(icon);
    return;
  }
  PickIcon(ev);
}

void SettingsProfileActions::PickIcon(ui::Event& ev) {
  if (host_.UiState().profile_icon_uploading) {
    return;
  }
  if (!commands_.pick_profile_icon_image || !commands_.upload_profile_icon_file) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile icon upload is not available"));
    return;
  }
  const ProfileIconSourcePlan plan =
      PlanProfileIconSources(Platform::IsMobile(), host_.UiState().profile_show_clear_icon);
  if (plan.run_directly) {
    RunIconSource(plan.actions.front());
    return;
  }
  std::vector<ContextMenuAction> actions;
  for (const ProfileIconSourceAction action : plan.actions) {
    ContextMenuAction item;
    item.run = [this, action]() { RunIconSource(action); };
    switch (action) {
      case ProfileIconSourceAction::Camera:
        item.id = "profile_photo_camera";
        item.label = Tr("chat.attach.capture_photo");
        item.icon = "../icons/v2/camera.svg";
        break;
      case ProfileIconSourceAction::Library:
        item.id = "profile_photo_library";
        item.label = Tr("settings.profile.photo_from_library");
        item.icon = "../icons/v2/photo.svg";
        break;
      case ProfileIconSourceAction::Remove:
        item.id = "profile_photo_remove";
        item.label = Tr("settings.profile.remove_photo");
        item.icon = "../icons/v2/trash.svg";
        item.danger = true;
        break;
    }
    actions.push_back(std::move(item));
  }
  ContextMenuAction cancel{"profile_photo_cancel", Tr("common.cancel"), nullptr, []() {}};
  cancel.cancel = true;
  actions.push_back(std::move(cancel));
  ContextMenuHost::Instance().ShowActions(MenuPositionBelowEvent(ev), std::move(actions));
}

void SettingsProfileActions::RunIconSource(const ProfileIconSourceAction action) {
  switch (action) {
    case ProfileIconSourceAction::Camera:
      ShowCameraCapture(Backend::GetWindow(), CameraCaptureKind::Photo, 0, 0,
                        [this](const CameraCaptureOutcome outcome, std::vector<std::string> paths) {
                          AppRuntime::PostUI([this, outcome, paths = std::move(paths)]() mutable {
                            if (outcome == CameraCaptureOutcome::Picked && !paths.empty()) {
                              IconChosen(std::move(paths.front()));
                            } else if (outcome == CameraCaptureOutcome::Unavailable ||
                                       outcome == CameraCaptureOutcome::Unprocessable) {
                              UserFeedback::Fail(Tr("chat.attach.camera_unavailable"));
                            }
                          });
                        });
      break;
    case ProfileIconSourceAction::Library:
      // The native picker opens at once (as chat's attach_file); the vault is asked for only after a
      // file is chosen, so the PIN prompt cannot keep the dialog from appearing.
      commands_.pick_profile_icon_image([this](std::vector<std::string> paths) {
        AppRuntime::PostUI([this, paths = std::move(paths)]() {
          if (paths.empty()) {
            if (const char* err = SDL_GetError(); err != nullptr && err[0] != '\0') {
              host_.ReportFailure(Error(err).WithUser(Tr("settings.profile.picker_failed")));
              SDL_ClearError();
            }
            return;
          }
          IconChosen(paths.front());
        });
      });
      break;
    case ProfileIconSourceAction::Remove:
      // Next to the change-photo rows a tap lands here too easily, so ask once.
      UserFeedback::ConfirmDestructive(
          Tr("settings.profile.photo_remove_confirm"), "",
          [this](const bool confirmed) {
            if (confirmed && !host_.UiState().profile_icon_uploading) {
              ClearIcon();
            }
          },
          Tr("common.remove"));
      break;
  }
}

void SettingsProfileActions::IconChosen(std::string path) {
  // From here the file is released by its lease: when the upload ends (either way), or when the PIN step lets go of
  // this closure unanswered, cancelled or failed.
  auto file = std::make_shared<PickedFileLease>(std::move(path));
  if (host_.UiState().profile_icon_uploading) {
    // A pick that came back late, while a change is already in flight: drop it (the lease releases the file).
    file->Release();
    return;
  }
  if (!commands_.open_avatar_crop) {
    Unlocked("PIN required to change profile icon", [this, file]() { UploadIcon(file, std::nullopt, nullptr); });
    return;
  }
  // The crop page comes first, and it holds the file until it closes. "Done" is what asks for the PIN; a refused PIN
  // or a failed upload leaves the page open (answered false), a finished upload closes it (answered true).
  commands_.open_avatar_crop(file, [this, file](ProfileIconCrop crop, AvatarCropReply reply) {
    auto answer = std::make_shared<AvatarCropFinish>(std::move(reply));
    Unlocked(
        "PIN required to change profile icon", [this, file, crop, answer]() { UploadIcon(file, crop, answer); },
        [answer]() { answer->Answer(false); });
  });
}

void SettingsProfileActions::UploadIcon(std::shared_ptr<PickedFileLease> file, std::optional<ProfileIconCrop> crop,
                                        std::shared_ptr<AvatarCropFinish> crop_finished) {
  if (host_.UiState().profile_icon_uploading) {
    // The PIN step can answer after another change started; never run two at once.
    if (crop_finished) {
      crop_finished->Answer(false);
    } else {
      file->Release();
    }
    return;
  }
  if (!commands_.plan_relay_quota_recovery || !commands_.delete_relay_file) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile icon upload is not available"));
    if (crop_finished) {
      crop_finished->Answer(false);
    }
    return;
  }
  host_.UiState().profile_icon_uploading = true;
  host_.PushToView(/*include_profile_nickname=*/false);
  if (crop_finished) {
    crop_finished->Uploading(true); // the crop page shows "Uploading…" from here (the PIN is done)
  }
  BlobQuotaRecoveryFlow::RunVoidUpload(
      [this, file, crop]() { return commands_.upload_profile_icon_file(file->path(), crop); },
      [this, file, crop_finished](Roe<void> result) {
        if (crop_finished) {
          // A failure keeps the file for a retry while the crop page is open; when the page is already gone (swept
          // mid-upload) the last reference dropped here releases it. Either way no explicit release under a reader.
          crop_finished->Answer(static_cast<bool>(result));
          if (result) {
            file->Release(); // the photo is up: the page closed (or is gone), nobody needs the file
          }
        } else {
          file->Release();
        }
        AfterIconChange(result, Tr("settings.profile.photo_updated"));
      },
      [this](const Error& quota_error) { return commands_.plan_relay_quota_recovery(quota_error); },
      [this](const std::string& blob_id) { return commands_.delete_relay_file(blob_id); });
}

void SettingsProfileActions::ClearIcon() {
  if (host_.UiState().profile_icon_uploading) {
    return;
  }
  if (!commands_.clear_profile_icon) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile icon upload is not available"));
    return;
  }
  Unlocked("PIN required to change profile icon", [this]() {
    if (host_.UiState().profile_icon_uploading) {
      return; // an upload started while the PIN was being entered
    }
    host_.UiState().profile_icon_uploading = true;
    host_.PushToView(/*include_profile_nickname=*/false);
    AppRuntime::PostWorkerNormal([this]() {
      Roe<void> result = commands_.clear_profile_icon();
      AppRuntime::PostUI(
          [this, result = std::move(result)]() { AfterIconChange(result, Tr("settings.profile.photo_removed")); });
    });
  });
}

void SettingsProfileActions::AfterIconChange(const Roe<void>& result, const std::string& done_message) {
  host_.UiState().profile_icon_uploading = false;
  host_.Resync(/*include_profile_nickname=*/false);
  if (!result) {
    // The white pill alone reports it; ReportFailure would also write the inline status line.
    logging::getLogger("SettingsProfile").warning << "Profile icon change failed: " << AppError::Log(result.error());
    UserFeedback::FailFrom(result.error());
    return;
  }
  UserFeedback::Ok(done_message);
}

void SettingsProfileActions::Share(const ShareAnchor anchor) {
  // The account id is what friends type to find you; without one there is nothing to share yet.
  host_.PullFromView();
  const SettingsUiState& state = host_.UiState();
  if (state.profile_relay_id.empty()) {
    UserFeedback::Fail(Tr("settings.profile.no_peer_id"));
    return;
  }
  ShareInviteOrCopy(
      AccountInviteText(state.profile_nickname, state.profile_relay_id, state.profile_contact_locator),
      [](const std::string& text, const ShareAnchor at) { return PlatformShareText(Backend::GetWindow(), text, at); },
      anchor,
      [](const std::string& text) {
        if (ui::SystemInterface* system = ui::GetSystemInterface()) {
          system->SetClipboardText(text.c_str());
        }
      });
}

} // namespace pbr
