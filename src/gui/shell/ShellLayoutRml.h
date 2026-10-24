#pragma once

#include "lib/ui/ShellTypes.h"

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

private:
  std::string PaneSlot(const std::string& key, const char* extra_class, bool with_composer_slot = false,
                       int flex_width_dp = 0) const;
  std::string ExpandedBase() const;
  std::string CompactBase() const;
  std::string AccountSheet() const;
  std::string TransientLayer() const;
  std::string Overlays() const;
  const PaneState* FindPane(const std::string& key) const;
  bool ChromeFrostEnabled() const;

  const ShellState& state_;
  bool bottom_chrome_open_;
  std::string bottom_chrome_key_;
};

} // namespace pbr
