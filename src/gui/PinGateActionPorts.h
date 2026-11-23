#pragma once

#include <functional>

namespace pbr {

/**
 * PIN gate action ports for ShellHost (no PinGateController*).
 * Application fills from PinGateController. Clear via BindPinGateActions({}).
 */
struct PinGateActionPorts {
  std::function<void()> on_submit;
  std::function<void()> on_cancel;
  std::function<void()> on_set_pin;
  std::function<void()> on_use_default;
  std::function<void()> on_identity_new;
  std::function<void()> on_identity_link;
  /** "Restore with my recovery phrase" (M023): a PIN for this device, then the 24 words. */
  std::function<void()> on_identity_phrase;
  /** The phrase page's "Use a saved copy…". */
  std::function<void()> on_use_saved_copy;
  std::function<void()> on_forgot_toggle;
  /** The "?" after the default-PIN line opens / closes its note. */
  std::function<void()> on_note_toggle;
  /** "Enter PIN instead" while the unlock screen waits for the phone's unlock (PIN grace window). */
  std::function<void()> on_use_pin_instead;
  /** The number pad (DEV/PLAN-pin-gate-redesign.md B): a digit, backspace, a new PIN's "OK", and the two links. */
  std::function<void(int digit)> on_key;
  std::function<void()> on_backspace;
  std::function<void()> on_keypad_ok;
  std::function<void()> on_use_text;
  std::function<void()> on_use_keypad;
  /** The pad's face / fingerprint key, or the field's "Unlock with Face ID" link (at-rest A014). */
  std::function<void()> on_biometric;
  /** "Enter PIN" on a biometric-first unlock door. */
  std::function<void()> on_use_pin;
  /** "Start over" (the profile-problem page, or under "Forgot PIN?"): its confirm step. */
  std::function<void()> on_start_over;
  /** The confirm step's "Start over": the reset runs. */
  std::function<void()> on_start_over_confirm;
  /** "Quit" on the profile-problem page (desktop). */
  std::function<void()> on_quit;
};

} // namespace pbr
