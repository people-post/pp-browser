#include "gui/chat/PasteTrigger.h"

#include "foundation/platform/Platform.h"

#include <ui/base/Input.h>
#include <ui/dom/Context.h>
#include <ui/dom/Element.h>

#include <cstring>
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
  // false = the key is taken (ProcessKeyDown stops); true = it goes on to the text field as it always did.
  binding.action = [ports]() { return !(ports.paste && ports.paste()); };
  binding.priority = 50;
  binding.skip_repeat = true;
  return binding;
}

std::function<bool(ui::Element* editor)> PasteMenuInterceptor(PasteTriggerPorts ports) {
  return [ports = std::move(ports)](ui::Element* editor) {
    return IsChatComposerField(editor, ports) && ports.paste && ports.paste();
  };
}

} // namespace pbr
