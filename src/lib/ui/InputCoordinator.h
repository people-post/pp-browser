#pragma once

#include <ui/base/Input.h>
#include <functional>
#include <vector>

namespace ui {
class Context;
}

namespace pbr {

struct KeyBinding {
  ui::Input::KeyIdentifier key = ui::Input::KI_UNKNOWN;
  int required_modifiers = 0;
  int forbidden_modifiers = 0;
  std::function<bool(ui::Context*)> when;
  std::function<bool()> action;
  int priority = 0;
  bool priority_phase = true;
  /** Not for a key the keyboard repeats (a key held down): the binding is skipped and the key goes on untouched. */
  bool skip_repeat = false;
  /**
   * With `skip_repeat`: asked instead of skipping when the key repeats (the binding still has to match: key, modifiers,
   * `when`). False = the repeat is taken as well (the key does not go on); true / unset = it goes on untouched.
   */
  std::function<bool()> repeat_action;
};

class InputCoordinator {
public:
  InputCoordinator() = default;

  void Register(KeyBinding binding);
  void Clear();

  /// Returns false when a binding consumed the key (stop further handling).
  bool ProcessKeyDown(ui::Context* context, ui::Input::KeyIdentifier key, int key_modifier, bool priority_phase,
                      bool repeat = false);

private:
  std::vector<KeyBinding> bindings_;
};

} // namespace pbr
