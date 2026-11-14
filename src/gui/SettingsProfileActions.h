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
#include <vector>
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
  ~SettingsProfileActions();

  /** Its fields and events in the settings model. */
  void Bind(ui::DataModelConstructor& ctor);
  /** The invite locator the profile shows now: its QR code follows (made again only when it changes). */
  void ShowInvite(const std::string& locator);
  /**
   * Register (or renew) the identity, unlocking first; the two requests run on a worker, the page stays usable and
   * the update button shows "Updating…" meanwhile. `quiet_success`: no "registered" / "renewed" line (the reconnect
   * row: "Reconnected" comes when a poll gets through), and a network failure says so in its own words.
   * `done(registered)` when it ends (false: the PIN was not given, or the registration failed or was refused, or a
   * messaging teardown (OnMessagingTornDown) ended it); exactly once.
   * One registration at a time, from the first tap on (the PIN step included): another button tap meanwhile does
   * nothing; a quiet caller meanwhile joins the one running and gets its outcome.
   */
  void Register(bool quiet_success = false, std::function<void(bool ok)> done = {});
  /** What a registration asks to hold the automatic renewal's slot: `run` gets the `release` it must call, once. */
  using RegistrationSlotRun = std::function<void(std::function<void()> release)>;
  /**
   * How a registration waits for the automatic renewal in flight (ChatController::RunWhenRegistrationIdle): it calls
   * its argument at once, or once that renewal ends, handing it the slot to release. Unbound: at once, nothing to
   * release.
   */
  void BindRunWhenRegistrationIdle(std::function<void(RegistrationSlotRun)> run);
  /** True from a registration's first tap until it ends (Me → Profile's nickname is not saved meanwhile). */
  bool RegistrationStarted() const { return started_; }
  /**
   * Messaging was torn down (profile reset): a reply the teardown dropped never comes, so what waited for it lets go:
   * the update button works again, every caller waiting for the outcome is answered false, and a late reply or a
   * start still queued behind the renewal does nothing.
   */
  void OnMessagingTornDown();
  /** The "QR code" button on the Me card opens the full-page "My QR code" (ContactsController::OpenMyQr). */
  void BindShowMyQr(std::function<void()> show);

private:
  /** Holding the renewal's slot (`release` it on every way out): the request, then its reply (OnRegistered). */
  void RegisterUnlocked(bool renewing, bool quiet_success, std::function<void()> release);
  /** The reply: says the result once (the failure, or "updated"), then ends the registration. */
  void OnRegistered(const Roe<void>& registered, bool renewing);
  /** Ends the registration: the flags are cleared, then every caller waiting is answered with `result`. */
  void Conclude(const Roe<void>& result);
  void SetRegistering(bool registering);
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

  /** Everyone waiting for the registration running (the first is the one that started it): quiet-ness and `done`. */
  struct Waiter {
    bool quiet = false;
    std::function<void(Roe<void>)> done;
  };

  std::function<void(RegistrationSlotRun)> run_when_registration_idle_;
  /** A registration is running, from the first tap (the PIN step included); C++ only. */
  bool started_ = false;
  /** Shown on the page: the update button reads "Updating…" and is greyed (from the PIN given until it ends). */
  bool registering_ = false;
  std::vector<Waiter> waiters_;
  /** Cleared when this goes, and counted up by a teardown: a reply that comes after either touches nothing. */
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
  uint64_t generation_ = 0;
  std::function<void()> show_my_qr_;
  std::string qr_locator_;
  ui::String qr_svg_rml_;
  bool show_qr_ = false;
};

} // namespace pbr
