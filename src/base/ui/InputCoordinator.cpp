#include "base/ui/InputCoordinator.h"

#include <RmlUi/Core/Context.h>

#include <algorithm>

namespace pbr {

void InputCoordinator::Register(KeyBinding binding) {
  bindings_.push_back(std::move(binding));
  std::stable_sort(bindings_.begin(), bindings_.end(),
                   [](const KeyBinding& a, const KeyBinding& b) { return a.priority > b.priority; });
}

void InputCoordinator::Clear() {
  bindings_.clear();
}

bool InputCoordinator::ProcessKeyDown(Rml::Context* context, Rml::Input::KeyIdentifier key, int key_modifier,
                                      bool priority_phase) {
  for (const KeyBinding& binding : bindings_) {
    if (binding.priority_phase != priority_phase) {
      continue;
    }
    if (binding.key != key) {
      continue;
    }
    if ((key_modifier & binding.required_modifiers) != binding.required_modifiers) {
      continue;
    }
    if ((key_modifier & binding.forbidden_modifiers) != 0) {
      continue;
    }
    if (binding.when && !binding.when(context)) {
      continue;
    }
    if (!binding.action) {
      continue;
    }
    if (!binding.action()) {
      return false;
    }
  }
  return true;
}

} // namespace pbr
