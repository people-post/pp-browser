#pragma once

#include <RmlUi/Core/Input.h>
#include <functional>
#include <vector>

namespace Rml {
class Context;
}

namespace pbr {

struct KeyBinding {
  Rml::Input::KeyIdentifier key = Rml::Input::KI_UNKNOWN;
  int required_modifiers = 0;
  int forbidden_modifiers = 0;
  std::function<bool(Rml::Context*)> when;
  std::function<bool()> action;
  int priority = 0;
  bool priority_phase = true;
};

class InputCoordinator {
public:
  InputCoordinator() = default;

  void Register(KeyBinding binding);
  void Clear();

  /// Returns false when a binding consumed the key (stop further handling).
  bool ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier, bool priority_phase);

private:
  std::vector<KeyBinding> bindings_;
};

} // namespace pbr
