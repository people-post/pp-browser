#pragma once

#include "gui/chat/ChatScreenState.h"
#include "lib/ui/MentionDraft.h"

#include <ui/dom/EventListener.h>

#include <functional>
#include <optional>
#include <string>

namespace ui {
class Context;
class Element;
class ElementFormControlTextArea;
} // namespace ui

namespace pbr {

class ChatPresenter;

/**
 * The composer's @ popup in group chats (G015): feeds the draft box's text and cursor to MentionDraft, shows its
 * rows in the chat model, writes its edits back, and hands the mentions to the send.
 *
 * What RmlUi reports (pp-cpp-ui, checked 2026-10-30): the text area counts its cursor in code points
 * (ElementFormControlTextArea::GetSelection); the Textinput event carries only committed characters, and the SDL
 * backend does not forward IME composition (no SDL text-editing events, IsComposing() is internal), so the value
 * is never a half-composed string here. "Committed" is therefore a Textinput event seen since the last tick; a
 * value change without one never opens the popup. MentionDraft also takes a `composing` flag should RmlUi ever
 * report one.
 */
class ChatMentionController : public ui::EventListener {
public:
  struct Ports {
    std::function<std::string()> thread_id;
  };

  ChatMentionController(ChatScreenState& state, ChatPresenter& presenter, ui::Context*& context, Ports ports);
  ~ChatMentionController() override;

  /** Once per frame (UI thread). */
  void Tick();
  /** A row was tapped. */
  void Pick(int row_index);
  /** The mentions payload JSON for the message about to go out; nullopt when there are none (plain text). */
  std::optional<std::string> PayloadForSend(const std::string& draft_text, const std::string& sent_text) const;

  void ProcessEvent(ui::Event& event) override;

private:
  ui::ElementFormControlTextArea* DraftBox() const;
  void Reconfigure(const std::string& thread_id);
  void Apply(const DraftEdit& edit);
  void SyncState();
  void Attach(ui::Element* document);

  ChatScreenState& state_;
  ChatPresenter& presenter_;
  ui::Context*& context_;
  Ports ports_;
  MentionDraft draft_;
  std::string thread_id_;
  bool committed_input_ = false;
  ui::Element* attached_document_ = nullptr;
};

} // namespace pbr
