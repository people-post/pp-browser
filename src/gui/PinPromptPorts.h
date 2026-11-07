#pragma once

#include "common/Error.h"
#include "common/Secret.h"

#include <functional>
#include "common/PbrCompat.h"

namespace pbr {

/** Why settings asks for the PIN (the gate's words follow; DEV/PLAN-pin-gate-redesign.md B2). */
enum class PinVerifyPurpose { ExportTransferCode, ShowRecoveryPhrase, EnableBiometric };

/**
 * Checks `pin` (off the UI thread if it must) and calls `done` on the UI thread: ok → the gate goes on; a wrong PIN →
 * the gate says so and stays; another error → its words on the gate.
 */
using PinGateCheck = std::function<void(const SecretText& pin, std::function<void(Roe<void>)> done)>;
/** Changes the PIN from `old_pin` to `new_pin` and calls `done` on the UI thread, as a check does. */
using PinGateChange =
    std::function<void(const SecretText& old_pin, const SecretText& new_pin, std::function<void(Roe<void>)> done)>;

/**
 * Settings → the PIN gate (no PinGateController*): every place that asks for the PIN goes through the gate, so a digit
 * PIN is typed on its keypad. Application fills from PinGateController. Clear via BindPinPrompt({}).
 */
struct PinPromptPorts {
  /** Ask once; `on_ok` runs after `check` passed and the gate closed (not after a cancel). */
  std::function<void(PinVerifyPurpose purpose, PinGateCheck check, std::function<void()> on_ok)> show_verify;
  /** Current PIN (checked by `check_old`) → new PIN → again → `change`. */
  std::function<void(PinGateCheck check_old, PinGateChange change)> show_change;
};

} // namespace pbr
