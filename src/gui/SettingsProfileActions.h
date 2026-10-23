#pragma once

#include "feature/settings/SettingsCommands.h"
#include "foundation/platform/PlatformShare.h"
#include "gui/SettingsHost.h"
#include "gui/UnlockEnsurePorts.h"

#include <ui/base/Types.h>

#include <string>
#include "common/PbrCompat.h"

namespace ui {
class DataModelConstructor;
}

namespace pbr {

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

private:
  void Register();
  void RotateBriefLlmKey();
  void CopyId();
  void PickIcon();
  void UploadIcon(const std::string& path);
  void ClearIcon();
  void Share(ShareAnchor anchor);
  void ToggleQr();
  /** Runs `then` once the vault is open (asks for the PIN when it is not); else reports `locked`. */
  void Unlocked(const char* locked, std::function<void()> then);
  /** After the photo changed (or failed to): the status line, the session's view of it. */
  void AfterIconChange(const Roe<void>& result, const std::string& done_message);

  SettingsHost& host_;
  const SettingsCommands& commands_;
  const UnlockEnsurePorts& unlock_;

  std::string qr_locator_;
  ui::String qr_svg_rml_;
  bool show_qr_ = false;
};

} // namespace pbr
