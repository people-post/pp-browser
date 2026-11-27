#include "gui/chat/PasteTrigger.h"

#include "foundation/platform/Platform.h"

#include <ui/base/Input.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>

#include <cstring>
#include <memory>
#include <utility>

namespace pbr {

bool IsChatComposerField(ui::Element* element, const PasteTriggerPorts& ports) {
  return element && element->GetId() == "draft-input" && ports.in_chat_composer_context &&
         ports.in_chat_composer_context();
}

int PasteKeyModifier() {
  return std::strcmp(Platform::OsName(), "macos") == 0 ? ui::Input::KM_META : ui::Input::KM_CTRL;
}

KeyBinding PasteKeyBinding(PasteTriggerPorts ports, const int modifier) {
  const int other = modifier == ui::Input::KM_META ? ui::Input::KM_CTRL : ui::Input::KM_META;
  KeyBinding binding;
  binding.key = ui::Input::KI_V;
  binding.required_modifiers = modifier;
  binding.forbidden_modifiers = ui::Input::KM_SHIFT | ui::Input::KM_ALT | other;
  binding.when = [ports](ui::Context* context) {
    return IsChatComposerField(context ? context->GetFocusElement() : nullptr, ports);
  };
  // Whether the last press (not a repeat) was taken over: its repeats are swallowed with it, else holding the key would
  // paste the copied file's NAME (Finder also puts it on the clipboard) through the text field.
  const auto taken = std::make_shared<bool>(false);
  // false = the key is taken (ProcessKeyDown stops); true = it goes on to the text field as it always did.
  binding.action = [ports, taken]() {
    *taken = ports.paste && ports.paste();
    return !*taken;
  };
  binding.priority = 50;
  binding.skip_repeat = true; // a repeat never pastes again
  binding.repeat_action = [taken]() { return !*taken; };
  return binding;
}

std::function<bool(ui::Element* editor)> PasteMenuInterceptor(PasteTriggerPorts ports) {
  return [ports = std::move(ports)](ui::Element* editor) {
    return IsChatComposerField(editor, ports) && ports.paste && ports.paste();
  };
}

} // namespace pbr
