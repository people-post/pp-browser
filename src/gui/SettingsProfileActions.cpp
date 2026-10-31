#include "gui/SettingsProfileActions.h"

#include "foundation/error/AppError.h"
#include "foundation/i18n/LocalizationService.h"
#include "foundation/platform/NativeFileDialog.h"
#include "foundation/platform/ui/RmlUi_Backend.h"
#include "common/runtime/AppRuntime.h"
#include "common/PbrCompat.h"
#include "gui/AccountInvite.h"
#include "gui/BlobQuotaRecoveryFlow.h"
#include "gui/UserFeedback.h"
#include "gui/shell/DataModelHost.h"
#include "lib/ui/DataEvent.h"
#include "lib/ui/QrSvg.h"
#include "lib/ui/UiEditSession.h"

#include <ui/base/SystemInterface.h>
#include <ui/data/DataModelHandle.h>
#include <SDL3/SDL.h>

#include <utility>
#include <vector>

namespace pbr {

SettingsProfileActions::SettingsProfileActions(SettingsHost& host, const SettingsCommands& commands,
                                               const UnlockEnsurePorts& unlock)
    : host_(host), commands_(commands), unlock_(unlock) {}

void SettingsProfileActions::Bind(ui::DataModelConstructor& ctor) {
  ctor.Bind("profile_qr_svg_rml", &qr_svg_rml_);
  ctor.Bind("profile_show_qr", &show_qr_);
  ctor.BindEventCallback("register_profile", DataEvent<>([this]() { Register(); }));
  ctor.BindEventCallback("rotate_brief_llm_key", DataEvent<>([this]() { RotateBriefLlmKey(); }));
  ctor.BindEventCallback("copy_profile_id", DataEvent<>([this]() { CopyId(); }));
  ctor.BindEventCallback("pick_profile_icon", DataEvent<>([this]() { PickIcon(); }));
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

void SettingsProfileActions::Unlocked(const char* locked, std::function<void()> then) {
  if (!unlock_.ensure_unlocked) {
    host_.ReportFailure(AppError::Pin(Err::Pin::Required, locked));
    return;
  }
  unlock_.ensure_unlocked([this, locked, then = std::move(then)](const bool unlocked) {
    if (!unlocked) {
      host_.ReportFailure(AppError::Pin(Err::Pin::Required, locked));
      return;
    }
    then();
  });
}

void SettingsProfileActions::Register() {
  host_.SaveProfileNickname();
  host_.PullFromView();
  const bool renewing = host_.UiState().profile_registered == "yes";
  Unlocked("PIN required to register", [this, renewing]() {
    if (!commands_.register_identity) {
      host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Registration is not available"));
      return;
    }
    if (auto registered = commands_.register_identity({.nickname = host_.UiState().profile_nickname});
        !registered) {
      host_.ReportFailure(registered.error());
      return;
    }
    if (commands_.load_profile_identity) {
      const ProfileIdentityView view = commands_.load_profile_identity();
      if (view.ready) {
        UiEditSession::Instance().OnCommitted(kUiFieldProfileNickname, view.nickname);
      }
    }
    const std::string message = renewing ? Tr("settings.registration.renewed") : Tr("settings.registration.done");
    host_.Resync();
    host_.ShowStatus(message);
    UserFeedback::Ok(message);
  });
}

void SettingsProfileActions::RotateBriefLlmKey() {
  host_.PullFromView();
  Unlocked("PIN required to rotate API key", [this]() {
    if (!commands_.rotate_brief_llm_key) {
      host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Key rotation is not available"));
      return;
    }
    if (auto rotated = commands_.rotate_brief_llm_key(); !rotated) {
      host_.Resync();  // the identity may have been marked expired (401 / 403)
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
  const std::string& peer_id = host_.UiState().profile_peer_id;
  if (peer_id.empty()) {
    UserFeedback::Fail(Tr("settings.profile.no_peer_id"));
    return;
  }
  if (ui::SystemInterface* system = ui::GetSystemInterface()) {
    system->SetClipboardText(peer_id.c_str());
  }
  UserFeedback::Ok(Tr("settings.profile.peer_id_copied"));
}

void SettingsProfileActions::PickIcon() {
  if (host_.UiState().profile_icon_uploading) {
    return;
  }
  if (!commands_.pick_profile_icon_image || !commands_.upload_profile_icon_file) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile icon upload is not available"));
    return;
  }
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
      const std::string path = paths.front();
      Unlocked("PIN required to change profile icon", [this, path]() { UploadIcon(path); });
    });
  });
}

void SettingsProfileActions::UploadIcon(const std::string& path) {
  if (!commands_.plan_relay_quota_recovery || !commands_.free_oldest_relay_blob_slot) {
    host_.ReportFailure(AppError::Storage(Err::Storage::Unavailable, "Profile icon upload is not available"));
    return;
  }
  host_.UiState().profile_icon_uploading = true;
  host_.PushToView(/*include_profile_nickname=*/false);
  host_.ShowStatus(Tr("settings.profile.photo_uploading"));
  BlobQuotaRecoveryFlow::RunVoidUpload(
      [this, path]() { return commands_.upload_profile_icon_file(path); },
      [this, path](Roe<void> result) {
        ReleasePickedFile(path);
        AfterIconChange(result, Tr("settings.profile.photo_updated"));
      },
      [this]() { return commands_.plan_relay_quota_recovery(); },
      [this]() { return commands_.free_oldest_relay_blob_slot(); });
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
    host_.UiState().profile_icon_uploading = true;
    host_.PushToView(/*include_profile_nickname=*/false);
    AppRuntime::PostWorkerNormal([this]() {
      Roe<void> result = commands_.clear_profile_icon();
      AppRuntime::PostUI([this, result = std::move(result)]() {
        AfterIconChange(result, Tr("settings.profile.photo_removed"));
      });
    });
  });
}

void SettingsProfileActions::AfterIconChange(const Roe<void>& result, const std::string& done_message) {
  host_.UiState().profile_icon_uploading = false;
  host_.ShowStatus(result ? done_message : std::string());
  host_.Resync(/*include_profile_nickname=*/false);
  if (!result) {
    host_.ReportFailure(result.error());
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
      anchor, [](const std::string& text) {
        if (ui::SystemInterface* system = ui::GetSystemInterface()) {
          system->SetClipboardText(text.c_str());
        }
      });
}

} // namespace pbr
