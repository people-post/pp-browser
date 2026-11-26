#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "gui/contacts/PeoplePickerChoose.h"

#include <functional>
#include <string>

namespace pbr {

/** What the forward flow reaches the screen through. ChatController fills it; a test fills it with fakes. */
struct ForwardFlowPorts {
  /** Opens the "Forward to" picker (the people picker's choose mode): `done` gets the ticked ones, `cancel` a plain close. */
  std::function<void(PeoplePickerChooseOptions options, PeoplePickerChooseDone done, std::function<void()> cancel)>
      open_picker;
  /** Closes the picker if it is open, without running either callback. */
  std::function<void()> close_picker;
  /** The bottom white pill. */
  std::function<void(const std::string& message)> pill;
  /** The selection (or whether there is one) changed: the rows' ticks and the bars are drawn again. */
  std::function<void()> changed;
};

/**
 * What a user sees of a forward, in order (DEV/PLAN-message-forward-text.md): "Select" in a message's menu starts choosing
 * messages in the chat (while "Forward" on a message goes straight to "Forward to"), a tap on a row ticks it, the bottom button opens "Forward to", its Send queues everything and the
 * chat is back with one pill saying what happened. Every decision is the presenter's (ChatPresenter); this is the thread
 * between it and the screen, and the one place that ends it all (lock, sign-out, the chat gone). UI thread.
 */
class ForwardFlow {
public:
  ForwardFlow(ChatPresenter& presenter, ForwardFlowPorts ports);

  /** The chat is choosing messages (the bars and ticks are up). A single forward is not choosing, whatever the presenter holds. */
  bool Selecting() const { return presenter_.ForwardChoosing() && !single_; }
  bool Ticked(const std::string& message_id) const;
  /** "3 selected". */
  std::string CountLabel() const;
  /** The bottom button works: at least one message is ticked and the picker is not already open. */
  bool CanNext() const;

  /**
   * The menu's "Forward" on `message_id`: that one message, "Forward to" opens at once, no bars or ticks are ever shown.
   * The presenter's selection only carries the message; Cancel, a refused picker, lock and sign-out all clear it.
   */
  void BeginSingle(const std::string& message_id);
  /** The menu's "Select" on `message_id`: starts choosing with it ticked. */
  void BeginSelecting(const std::string& message_id);
  /** A tap on a row while choosing: ticks or unticks it, or says (pill) why not. */
  void Tap(const std::string& message_id);
  /** The top bar's Cancel. */
  void Cancel();
  /** The bottom button: opens "Forward to". */
  void Next();

  /** The app locked, or someone signed out: stops a forward being sent, closes the picker, leaves choosing. */
  void Stop();
  /**
   * Choosing ends and the picker closes, but a forward already being sent goes on (another chat opened, the chat being
   * chosen from went).
   */
  void Leave();
  /** The chat `thread_id` was deleted or left: if it is the one being chosen from, choosing ends and the bars are redrawn. */
  void OnThreadGone(const std::string& thread_id);
  /** A chat is on screen now: a forward that was stopped part-way is told once (to the account it happened under). */
  void OnChatEntered();

private:
  void Show(const std::string& message);
  void Changed();
  /** Opens "Forward to" for what the presenter holds ticked. */
  void OpenPicker();
  /** Begins the presenter's selection on `message_id` for either path; false (with the pill) when it cannot. */
  bool Start(const std::string& message_id);
  void OnChosen(std::vector<ForwardTarget> chosen);

  ChatPresenter& presenter_;
  ForwardFlowPorts ports_;
  bool picker_open_ = false;
  /** The selection is only carrying one message to the picker (BeginSingle): the screen shows no choosing. */
  bool single_ = false;
  /** The chat the messages are being chosen from (kept while the picker is open, after the presenter forgot it). */
  std::string source_thread_;
  /** A picker opened and then closed or answered belongs to an older flow: its late answer does nothing. */
  uint64_t picker_turn_ = 0;
};

/**
 * The one line after a forward: "Forwarded", the same with how many messages / chats were left out (both sentences when
 * both happened), or "Couldn't forward" when nothing went out.
 */
std::string ForwardResultText(const ForwardResult& result);

} // namespace pbr
