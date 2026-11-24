#pragma once

#include "common/Secret.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** What the new device's link page shows (M027): the offer as a QR code, as text, and its 4 digits. */
struct LinkOfferDisplay {
  std::string qr_svg;
  std::string text;
  std::string code;
};

/**
 * Unlock-gate completion ports for PinGateController (no ProfileUnlockGate*).
 * Application fills from ProfileUnlockGate. Clear via BindGateComplete({}).
 */
struct UnlockGateCompletePorts {
  std::function<void(const SecretText& pin, bool create_mode)> complete_with_pin;
  std::function<void()> complete_with_default_pin;
  /** Link this device (M027): the main device's reply as scanned or pasted, imported under `pin`, then unlock. */
  std::function<void(const SecretText& pin, const std::string& reply, bool set_default_pin)> complete_link_device;
  /**
   * The link page opens: a fresh one-time offer (the one before is wiped) — its QR code as SVG content, the same as
   * text, and its 4 digits; an error when none could be made.
   */
  std::function<Roe<LinkOfferDisplay>()> begin_link_offer;
  /** The person left the link page: the one-time key goes. */
  std::function<void()> forget_link_offer;
  /**
   * The camera reads the main device's reply: `done(text)` with what it read, or `closed()` when it read nothing
   * (cancelled, refused); either once, on the UI thread. Unset: no camera on this page.
   */
  std::function<void(std::function<void(std::string text)> done, std::function<void()> closed)> scan_link_reply;
  /**
   * Restore from a recovery phrase (M023) under `pin` for this device, then unlock; `saved_copies`: saved copies of
   * the account the user picked (exported locker files' text).
   */
  std::function<void(const SecretText& pin, const SecretText& phrase, bool set_default_pin,
                     const std::vector<std::string>& saved_copies)>
      complete_phrase_restore;
  /**
   * Let the user pick a saved copy of their account: `picked(name, text)` on the UI thread — an empty name when
   * nothing was picked, no text when the file could not be read.
   */
  std::function<void(std::function<void(std::string name, std::optional<std::string> text)> picked)> pick_saved_copy;
  std::function<void()> cancel;
  /** ProfileUnlockGate::UseDevicePinInstead: stop waiting for the phone's unlock, type the PIN. */
  std::function<void()> use_device_pin_instead;
  /** ProfileUnlockGate::TryBiometric: the face / fingerprint prompt again (the door's key). */
  std::function<void()> try_biometric;
};

} // namespace pbr
