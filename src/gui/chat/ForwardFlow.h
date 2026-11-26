#pragma once

#include "feature/presenters/ChatPresenter.h"
#include "gui/chat/ForwardPreview.h"
#include "gui/contacts/PeoplePickerChoose.h"

#include <functional>
#include <string>

namespace pbr {

/** What the progress page shows (DEV/PLAN-message-forward-files-v3.md, design 3): the words, ready to draw. */
struct ForwardProgressView {
  /** "Forwarding 2/7". */
  std::string title;
  /** What the current item is doing: a percent, "Sending… 12s", "One moment, almost there", "Finishing — …". */
  std::string line;
  /** 0-100 while the current upload reports its size; -1 when there is no figure (the page shows activity). */
  int percent = -1;
  /** Cancel was pressed: the Cancel button is gone, the item under way is being waited for. */
  bool finishing = false;
  /** Cancel got no answer for 15 s: "Close" is offered. */
  bool can_close = false;
};

/** The page's words for `progress` at `now_ms` (a pure function: the page is drawn from this). */
ForwardProgressView MakeForwardProgressView(const ForwardProgress& progress, int64_t now_ms);

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

  // --- Attachments (DEV/PLAN-message-forward-files-v3.md). Unset ports do nothing. ------------------------------------
  /** The progress page: a full-screen layer over the chat, drawn again with new numbers, closed. */
  std::function<void()> open_progress;
  std::function<void()> close_progress;
  std::function<void(const ForwardProgressView& view)> show_progress;
  /** The screen is kept on while the page is up (true), and let go on every exit (false). */
  std::function<void(bool on)> keep_awake;
  /** Monotonic milliseconds. */
  std::function<int64_t()> now_ms;
  /** What "Forward to" shows of attachments (thumbnails, posters). */
  ForwardPreviewPorts preview;
  /** "Forward to" is over (sent, cancelled, closed by the host): the files `preview.put_file` wrote go. */
  std::function<void()> preview_done;
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
  ~ForwardFlow();

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

  // --- The progress page (a forward with attachments) ---
  /** The page is up (its layer is open and the screen is held on). */
  bool ProgressOpen() const { return progress_open_; }
  /** Every UI tick: the page's numbers, the page leaving when the run is gone, a stopped forward's notice once its item answered. */
  void Tick(bool chat_visible);
  /** The page's Cancel button. */
  void OnProgressCancel();
  /** The page's Close button (after a Cancel that got no answer for 15 s). */
  void OnProgressClose();
  /** The system back key or Esc on the page: a Cancel, consumed (the page stays until the item under way answers). */
  bool OnProgressBack();
  /** The layer was closed by someone else (a tapped notification clears them all): handled like a Cancel. */
  void OnProgressLayerClosed();
  /** The phone went to the background proper (not focus loss): stops at once, the page goes, the item under way is not cut. */
  void OnAppEnteredBackground();
  /** A call is on (answered or placed): its controls sit under the page, so the page goes and the run stops as in the background. */
  void OnCallStarted();

private:
  void Show(const std::string& message);
  void Changed();
  /** Opens "Forward to" for what the presenter holds ticked. */
  void OpenPicker();
  /** Begins the presenter's selection on `message_id` for either path; false (with the pill) when it cannot. */
  bool Start(const std::string& message_id);
  void OnChosen(std::vector<ForwardTarget> chosen);
  void OnForwardDone(const ForwardResult& result);
  void OpenProgress();
  void CloseProgress();
  void ReleaseAwake();
  void PickerClosed();
  /** Says what a stopped forward did, once its item under way has answered (or tells that one is still sending). */
  void TryNotice();

  ChatPresenter& presenter_;
  ForwardFlowPorts ports_;
  bool picker_open_ = false;
  /** The selection is only carrying one message to the picker (BeginSingle): the screen shows no choosing. */
  bool single_ = false;
  /** The chat the messages are being chosen from (kept while the picker is open, after the presenter forgot it). */
  std::string source_thread_;
  /** A picker opened and then closed or answered belongs to an older flow: its late answer does nothing. */
  uint64_t picker_turn_ = 0;
  bool progress_open_ = false;
  bool awake_ = false;
  /** The "one is still sending" line was said for the notice that is waiting. */
  bool still_sending_said_ = false;
};

/**
 * The one line after a forward: "Forwarded", the same with how many messages / chats were left out (both sentences when
 * both happened), or "Couldn't forward" when nothing went out.
 */
std::string ForwardResultText(const ForwardResult& result);

} // namespace pbr
