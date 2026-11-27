#pragma once

#include "lib/ui/InputCoordinator.h"

#include <functional>

namespace ui {
class Element;
}

namespace pbr {

/**
 * The two ways a paste reaches the chat on a computer (DEV/PLAN-desktop-drop-paste.md, D1): the key (Ctrl+V; Cmd+V on a
 * Mac) and the context menu's Paste. Both ask the same two things of the chat screen; only the chat's own message box
 * qualifies — never Home's or an AI conversation's box (the same id), the AI half-panel, a message being edited, a
 * search field or a settings field. Anything else is left exactly as it is: the key reaches the text field.
 */
struct PasteTriggerPorts {
  /** The open conversation is a chat (not Home, not an AI's) and no message is being edited. */
  std::function<bool()> in_chat_composer_context;
  /** Tries to paste a picture; true = handled (the text paste must not run). Never called for another field. */
  std::function<bool()> paste;
};

/** The chat's message box: `draft-input` while `in_chat_composer_context` holds. The AI half-panel's `ask-ai-input` and every other field are not. */
bool IsChatComposerField(ui::Element* element, const PasteTriggerPorts& ports);

/** The modifier of a paste on this OS: Cmd (meta) on a Mac, Ctrl elsewhere. */
int PasteKeyModifier();

/** V with `modifier` (and no Shift / Alt / the other of Ctrl and Meta) while the focus is the chat's message box. A repeat never pastes again: it is swallowed when the press it follows was taken over, and goes on to the field otherwise. */
KeyBinding PasteKeyBinding(PasteTriggerPorts ports, int modifier);

/** For ContextMenuHost::SetPasteInterceptor. */
std::function<bool(ui::Element* editor)> PasteMenuInterceptor(PasteTriggerPorts ports);

} // namespace pbr
