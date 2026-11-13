#pragma once

#include "feature/settings/SettingsCommands.h"
#include "foundation/platform/PickedFileLease.h"
#include "foundation/platform/PlatformShare.h"
#include "gui/ProfileIconSource.h"
#include "gui/SettingsHost.h"
#include "gui/UnlockEnsurePorts.h"

#include <ui/base/Types.h>
#include <ui/dom/Event.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

struct AvatarCropFinish;

/**
 * Me → Profile's actions: register (or renew) the identity, rotate the Brief API key, copy the id,
 * change or clear the photo, share the invite, and show the invite as a QR code (its fields are this
 * part's). The vault-guarded ones unlock first.
 *
 * UI thread.
 */
class SettingsProfileActions {
public:
  SettingsProfileActions(SettingsHost& host, const SettingsCommands& commands, const UnlockEnsurePorts& unlock);

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The invite locator the profile shows now: its QR code follows (made again only when it changes). */
  void ShowInvite(const std::string& locator);
  /**
   * Register (or renew) the identity, unlocking first. `quiet_success`: no "registered" / "renewed" line (the
   * reconnect row: "Reconnected" comes when a poll gets through), and a network failure says so in its own words.
   * `done(registered)` when it ends (false: the PIN was not given, or the registration failed or was refused); not
   * called when what it waited for is dropped by a teardown.
   */
  void Register(bool quiet_success = false, std::function<void(bool ok)> done = {});
  /**
   * How a registration waits for the automatic renewal in flight (ChatController::RunWhenRegistrationIdle): it calls
   * its argument at once, or once that renewal ends. Unbound: at once.
   */
  void BindRunWhenRegistrationIdle(std::function<void(std::function<void()>)> run);
  /** The "QR code" tile on the Me card opens the full-page "My QR code" (ContactsController::OpenMyQr). */
  void BindShowMyQr(std::function<void()> show);
  /** The "Share my card" tile: shown only while this is bound (it sends my card into a chat). */
  void BindShareOwnCard(std::function<void()> share);

private:
  /** False when it did not register (the failure is reported). */
  bool RegisterUnlocked(bool renewing, bool quiet_success);
  void RotateBriefLlmKey();
  void CopyId();
  /** The photo menu (camera, library, remove); where it has one choice only, that choice at once. */
  void PickIcon(ui::Event& ev);
  /** A tap on the photo: view it large when there is one (phone), else the same menu as "Change photo". */
  void TapAvatar(ui::Event& ev);
  void RunIconSource(ProfileIconSourceAction action);
  /** A photo came from the camera or the library: unlock, then upload it. The file is released whichever way this ends. */
  void IconChosen(std::string path);
  /**
   * Uploads the photo. With `crop_finished` (the crop page chose it) the page holds the file and is answered when this
   * ends; without it the file is released here.
   */
  void UploadIcon(std::shared_ptr<PickedFileLease> file, std::optional<ProfileIconCrop> crop,
                  std::shared_ptr<AvatarCropFinish> crop_finished);
  void ClearIcon();
  void Share(ShareAnchor anchor);
  void ToggleQr();
  /** Runs `then` once the vault is open (asks for the PIN when it is not); else reports `locked`. */
  void Unlocked(const char* locked, std::function<void()> then, std::function<void()> not_unlocked = {});
  /** After the photo changed (or failed to): the status line, the session's view of it. */
  void AfterIconChange(const Roe<void>& result, const std::string& done_message);

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const UnlockEnsurePorts& unlock_;

  std::function<void(std::function<void()>)> run_when_registration_idle_;
  std::function<void()> show_my_qr_;
  std::function<void()> share_own_card_;
  bool share_card_shown_ = false;
  std::string qr_locator_;
  ui::String qr_svg_rml_;
  bool show_qr_ = false;
};

} // namespace pbr
