#pragma once

#include "lib/ui/ShellTypes.h"

#include <sstream>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The shell's markup from its state, bound to the window model: the base layout (expanded or
 * compact) with its account sheet, the transient layer, overlays, the bottom chrome, the dialog and
 * the PIN gate. What ShellHost mounts; made fresh from the state each time.
 */
class ShellLayoutRml {
public:
  ShellLayoutRml(const ShellState& state, bool bottom_chrome_open, std::string bottom_chrome_key);

  /** The whole shell: base layout, transient layer, overlays, dialog. */
  std::string ShellRoot() const;
  std::string BottomChrome() const;
  std::string Dialog() const;
  std::string PinGate() const;
  /**
   * Phone AI conversation list (DEV/PLAN-minimal-nav.md C): the drawer layer while `ai_drawer_open`, else empty. It
   * goes into #shell-ai-drawer-host (ShellRoot), above the tab bar, which stays under its scrim; ShellHost mounts the
   * `ai_drawer` view into #shell-ai-drawer-mount. Never on the desktop.
   */
  std::string AiDrawer() const;
  /** True when a Cancel + OK pair would not fit side by side in the phone alert card: the buttons stack instead. */
  static bool DialogButtonsStacked(const std::string& cancel_label, const std::string& ok_label);
  /** The PIN gate's field (field mode), focused when the gate is mounted. */
  static constexpr const char* kPinGateFieldId = "shell-pin-gate-pin";
  /**
   * The gate shows a PIN field to type in (unlock / create / verify / change in field mode, not the number pad): it
   * gets the focus as the gate comes up, so the system keyboard is there without a tap (Kenneth 2026-11-07).
   */
  static bool PinGateFocusesField(const PinGateState& gate);

private:
  std::string PaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot = false,
                       int flex_width_dp = 0) const;
  std::string ExpandedBase() const;
  std::string CompactBase() const;
  std::string TransientLayer() const;
  std::string Overlays() const;
  const PaneState* FindPane(const std::string& key) const;
  void PinGateFields(std::ostringstream& out) const;
  void PinGateKeypad(std::ostringstream& out) const;
  void PinGateBiometricFirst(std::ostringstream& out) const;
  bool ChromeFrostEnabled() const;

  const ShellState& state_;
  bool bottom_chrome_open_;
  std::string bottom_chrome_key_;
};

} // namespace pbr
