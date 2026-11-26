#pragma once

namespace pbr {

/**
 * The PIN behind **"No PIN"** — the translation layer between what the person sees and what the code does.
 *
 * The person sees two states only: no PIN, or their own PIN. The wording never mentions a default PIN or its digits
 * (#780). In code there is always a PIN: the vault wraps the profile's data key (DEK) with a PIN-derived key, and a
 * PIN the app knows is what lets it open the vault by itself — at start without asking, and in the background on an
 * iPhone, where an incoming call can then ring through CallKit (VoIP push is bound only on this PIN:
 * ChatController, IPushDeviceRegistrar::SetVoipPushWanted).
 *
 * So "No PIN" is this well-known PIN, and `pin_is_default` (preferences) is how the code tells the two states apart:
 * - choosing no PIN ("Just continue" at first run, or at a link) seals the vault with it: ProfileUnlockGate::
 *   CompleteWithDefaultPin, PinGateController (the link's no-PIN choice);
 * - "Remove PIN" changes the person's PIN back to it: SettingsSecurityActions::RemovePin;
 * - changing the PIN while on no PIN uses it as the current PIN, unasked: SettingsSecurityActions::ChangePin;
 * - the UI says "No PIN" while `pin_is_default` is true: SecuritySettingsSection.
 * It protects nothing against someone holding the device (the captions say so); it is not a secret and is never shown.
 */
inline constexpr const char* kDefaultProfilePin = "123456";

} // namespace pbr
