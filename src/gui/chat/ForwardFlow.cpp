#include "gui/chat/ForwardFlow.h"

#include "foundation/i18n/LocalizationService.h"

#include <algorithm>
#include <map>
#include <utility>

namespace pbr {

namespace {

/** `text` without the leading `done` word and the punctuation after it ("Forwarded. 2 chats…" -> "2 chats…"). */
std::string AfterDoneWord(const std::string& text, const std::string& done, std::string& joiner) {
  std::string rest = text.rfind(done, 0) == 0 ? text.substr(done.size()) : text;
  joiner = " ";
  size_t skip = 0;
  while (skip < rest.size()) {
    if (rest.compare(skip, 3, "\xEF\xBC\x8C") == 0) { // "，"
      joiner = "\xEF\xBC\x8C";
      skip += 3;
    } else if (rest[skip] == '.' || rest[skip] == ',' || rest[skip] == ' ') {
      skip += 1;
    } else {
      break;
    }
  }
  return rest.substr(skip);
}

} // namespace

ForwardProgressView MakeForwardProgressView(const ForwardProgress& progress, const int64_t now_ms) {
  ForwardProgressView view;
  const size_t shown = progress.finishing ? progress.done : std::min(progress.done + 1, progress.total);
  view.title =
      Tr("chat.forward.progress_title", {{"done", std::to_string(shown)}, {"total", std::to_string(progress.total)}});
  view.finishing = progress.finishing;
  view.can_close = progress.finishing && progress.can_close;
  if (progress.finishing) {
    view.line = Tr("chat.forward.progress_finishing");
  } else if (progress.waiting_for_pace) {
    view.line = Tr("chat.forward.progress_wait");
  } else if (progress.uploading && progress.bytes_total > 0 && progress.bytes_done > 0) {
    view.percent = static_cast<int>(std::min<uint64_t>(100, progress.bytes_done * 100 / progress.bytes_total));
    // The amount is in the ring on the page: no second "N%" under it.
  } else if (progress.uploading) {
    const int64_t seconds = std::max<int64_t>(0, (now_ms - progress.uploading_since_ms) / 1000);
    view.line = Tr("chat.forward.progress_sending", {{"s", std::to_string(seconds)}});
  }
  return view;
}

namespace {

std::string NoticeText(const ChatPresenter::ForwardNotice& notice) {
  if (notice.total == 0) {
    return Tr("chat.forward.interrupted");
  }
  if (notice.done >= notice.total) {
    return Tr("chat.forward.done"); // the item that was under way landed and it was the last: all of it went
  }
  const std::map<std::string, std::string> counts = {{"done", std::to_string(notice.done)},
                                                     {"total", std::to_string(notice.total)}};
  return Tr(notice.kind == ForwardStop::Tapped ? "chat.forward.stopped" : "chat.forward.partial", counts);
}

} // namespace

std::string ForwardResultText(const ForwardResult& result) {
  switch (result.refusal) {
    case ForwardRefusal::None:
      break;
    default: {
      // Not started: the same line the picker gave above its Send button.
      ForwardCheck why;
      why.refusal = result.refusal;
      return ForwardRefusalText(why); // the figures are the picker's; this is only the safety net
    }
  }
  if (result.stop == ForwardStop::Cancelled || result.stop == ForwardStop::Abandoned ||
      result.stop == ForwardStop::Tapped) {
    if (result.forwarded < result.total_items) {
      return NoticeText({result.stop, result.forwarded, result.total_items});
    }
  }
  if (result.nothing_sent) {
    return Tr("chat.forward.failed");
  }
  const size_t left_out_messages = result.skipped_messages + result.send_errors;
  const std::string messages =
      left_out_messages > 0 ? Tr("chat.forward.done_skipped_messages", {{"n", std::to_string(left_out_messages)}})
                            : std::string();
  const std::string chats = result.skipped_targets > 0 ? Tr("chat.forward.done_skipped_targets",
                                                            {{"n", std::to_string(result.skipped_targets)}})
                                                       : std::string();
  if (messages.empty() && chats.empty()) {
    return Tr("chat.forward.done");
  }
  if (messages.empty() || chats.empty()) {
    return messages.empty() ? chats : messages;
  }
  std::string joiner;
  const std::string tail = AfterDoneWord(chats, Tr("chat.forward.done"), joiner);
  return messages + joiner + tail;
}

ForwardFlow::ForwardFlow(ChatPresenter& presenter, ForwardFlowPorts ports)
    : presenter_(presenter), ports_(std::move(ports)) {
  // A forward stopped because its source chat was deleted elsewhere says so now, where the user is looking.
  presenter_.SetForwardNoticeSink([this](const ChatPresenter::ForwardNotice& notice) { Show(NoticeText(notice)); });
}

ForwardFlow::~ForwardFlow() {
  presenter_.SetForwardNoticeSink({});
  ReleaseAwake();
}

bool ForwardFlow::Ticked(const std::string& message_id) const {
  const std::vector<std::string>& ids = presenter_.ForwardSelectedIds();
  return std::find(ids.begin(), ids.end(), message_id) != ids.end();
}

std::string ForwardFlow::CountLabel() const {
  return Tr("chat.forward.selected_count", {{"n", std::to_string(presenter_.ForwardSelectedIds().size())}});
}

bool ForwardFlow::CanNext() const {
  return Selecting() && !presenter_.ForwardSelectedIds().empty() && !picker_open_;
}

void ForwardFlow::Show(const std::string& message) {
  if (ports_.pill) {
    ports_.pill(message);
  }
}

void ForwardFlow::Changed() {
  if (ports_.changed) {
    ports_.changed();
  }
}

bool ForwardFlow::Start(const std::string& message_id) {
  if (presenter_.Forwarding()) {
    Show(Tr("chat.send.still_trying")); // the earlier forward is still being queued: the reason, not "cannot"
    return false;
  }
  if (!presenter_.BeginForwardSelection(message_id)) {
    Show(Tr("chat.forward.cannot"));
    return false;
  }
  source_thread_ = presenter_.ForwardSourceThread();
  return true;
}

void ForwardFlow::BeginSelecting(const std::string& message_id) {
  if (!Start(message_id)) {
    return;
  }
  single_ = false;
  Changed();
}

void ForwardFlow::BeginSingle(const std::string& message_id) {
  if (!Start(message_id)) {
    return;
  }
  single_ = true;
  OpenPicker();
}

void ForwardFlow::Tap(const std::string& message_id) {
  if (single_) {
    return;
  }
  switch (presenter_.ToggleForwardSelection(message_id)) {
    case ChatPresenter::ForwardTick::Selected:
    case ChatPresenter::ForwardTick::Deselected:
      Changed();
      return;
    case ChatPresenter::ForwardTick::NotForwardable:
      Show(Tr("chat.forward.cannot"));
      return;
    case ChatPresenter::ForwardTick::LimitReached:
      Show(Tr("chat.forward.limit_messages"));
      return;
    case ChatPresenter::ForwardTick::NotChoosing:
      return;
  }
}

void ForwardFlow::Cancel() {
  if (single_) {
    return; // the picker's own Cancel ends a single forward
  }
  presenter_.EndForwardSelection(); // only the choosing: a forward already being sent goes on
  Changed();
}

void ForwardFlow::Next() {
  if (!CanNext()) {
    return;
  }
  OpenPicker();
}

void ForwardFlow::OpenPicker() {
  if (!ports_.open_picker) {
    if (single_) {
      presenter_.EndForwardSelection();
      single_ = false;
    }
    return;
  }
  PeoplePickerChooseOptions options;
  options.title = Tr("chat.forward.pick_title");
  options.send_label = Tr("picture_send.send_one"); // the app's plain "Send"
  const auto count_text = [](const size_t count) {
    return Tr("chat.forward.preview_count", {{"n", std::to_string(count)}});
  };
  // What is chosen, before any chat is: the tiles and the note (attachments), the words (text and cards).
  // The picker opens at once: a picture that is not on the page shows its icon until its thumbnail is made off this thread.
  const ForwardCheck check = presenter_.CheckForward();
  ForwardPreviewPorts quick = ports_.preview;
  quick.plaintext = nullptr;
  quick.thumbnail = nullptr;
  ForwardPreviewContent content = BuildForwardPreview(check, presenter_.ForwardPreview(count_text), count_text, quick);
  std::vector<ForwardThumbJob> thumb_jobs = ForwardThumbJobsFor(check, content);
  std::vector<ChoosePreviewItem> tiles = content.items;
  options.preview = std::move(content.text);
  options.preview_items = std::move(content.items);
  options.preview_note = std::move(content.note);
  // The limits do not depend on the chats ticked (only the cap of chats does): Send is off with the reason showing.
  options.refusal = [this](const size_t) { return ForwardRefusalText(presenter_.CheckForward()); };
  options.limit_message = Tr("chat.forward.limit_targets");
  options.max_targets = kMaxForwardTargets;
  options.list = [this]() { return presenter_.ForwardTargets(); };
  picker_open_ = true;
  const uint64_t turn = ++picker_turn_;
  Changed();
  ports_.open_picker(
      std::move(options),
      [this, turn](std::vector<ForwardTarget> chosen) {
        if (turn == picker_turn_) {
          OnChosen(std::move(chosen));
        }
      },
      [this, turn]() {
        if (turn ==
            picker_turn_) { // closed without choosing: the messages stay ticked (a single forward forgets its one)
          picker_open_ = false;
          PickerClosed();
          if (single_) {
            single_ = false;
            presenter_.EndForwardSelection();
          }
          Changed();
        }
      });
  if (picker_open_ && turn == picker_turn_) { // a picker that was refused at once has nothing to fill
    StartThumbnails(turn, std::move(tiles), std::move(thumb_jobs));
  }
}

void ForwardFlow::StartThumbnails(const uint64_t turn, std::vector<ChoosePreviewItem> items,
                                  std::vector<ForwardThumbJob> jobs) {
  if (jobs.empty()) {
    return;
  }
  ForwardPreviewPorts work_ports = ports_.worker_preview ? ports_.worker_preview() : ports_.preview;
  // Runs on the UI thread. The worker below never touches `this`: it only hands this to `post_ui`.
  std::function<void(std::vector<ForwardThumbResult>)> deliver =
      [this, turn, items = std::move(items), alive = std::weak_ptr<int>(alive_)](
          std::vector<ForwardThumbResult> results) { // NOLINT(performance-unnecessary-value-param)
        if (alive.lock()) {
          OnThumbnails(turn, items, results);
        }
      };
  if (!ports_.post_worker) {
    deliver(MakeForwardThumbs(jobs, work_ports));
    return;
  }
  if (!ports_.post_ui) {
    return;
  }
  ports_.post_worker([jobs = std::move(jobs), work_ports = std::move(work_ports), deliver = std::move(deliver),
                      post_ui = ports_.post_ui]() mutable {
    std::vector<ForwardThumbResult> results = MakeForwardThumbs(jobs, work_ports);
    post_ui([deliver = std::move(deliver), results = std::move(results)]() mutable { deliver(std::move(results)); });
  });
}

void ForwardFlow::OnThumbnails(const uint64_t turn, std::vector<ChoosePreviewItem> items,
                               const std::vector<ForwardThumbResult>& results) {
  if (!picker_open_ || turn != picker_turn_) {
    return; // closed, chosen, locked, signed out or opened again since: nothing was written, so nothing is left
  }
  bool any = false;
  for (const ForwardThumbResult& result : results) {
    if (result.tile >= items.size() || !ports_.preview.put_file) {
      continue;
    }
    std::string path = ports_.preview.put_file(result.jpeg, result.tile);
    if (!path.empty()) {
      items[result.tile].thumb_src = std::move(path);
      any = true;
    }
  }
  if (any && ports_.update_picker_preview) {
    ports_.update_picker_preview(std::move(items));
  }
}

void ForwardFlow::OnChosen(std::vector<ForwardTarget> chosen) {
  picker_open_ = false;
  ++picker_turn_;
  PickerClosed();
  if (!presenter_.ForwardChoosing() || chosen.empty()) {
    if (single_) {
      single_ = false;
      presenter_.EndForwardSelection();
    }
    Changed();
    return;
  }
  // The picker keeps Send off while a limit is broken; this is for an answer that slipped through (a send that began
  // while it was open). The selection stays so the person can take something away and go again.
  const ForwardCheck check = presenter_.CheckForward();
  if (check.refusal != ForwardRefusal::None) {
    Show(ForwardRefusalText(check));
    if (single_) {
      single_ = false;
      presenter_.EndForwardSelection();
    }
    Changed();
    return;
  }
  const bool page =
      check.uploads > 0; // the same test as the call refusal: text, cards and attachments held nowhere never show one
  single_ = false;       // the presenter ends the selection itself; a multi-select was never "single"
  presenter_.ForwardSelectedTo(std::move(chosen), [this](const ForwardResult& result) { OnForwardDone(result); });
  if (page && presenter_.Forwarding()) {
    OpenProgress();
  }
  Changed(); // choosing is over at once; the pill follows when everything is queued
}

void ForwardFlow::OnForwardDone(const ForwardResult& result) {
  CloseProgress();
  Show(ForwardResultText(result));
}

void ForwardFlow::PickerClosed() {
  if (ports_.preview_done) {
    ports_.preview_done();
  }
}

void ForwardFlow::OpenProgress() {
  if (progress_open_) {
    return;
  }
  progress_open_ = true;
  if (ports_.keep_awake && !awake_) {
    awake_ = true;
    ports_.keep_awake(true); // a locked phone sends nothing: the page keeps the screen on
  }
  if (ports_.open_progress) {
    ports_.open_progress();
  }
  Tick(true);
}

void ForwardFlow::CloseProgress() {
  const bool was_open = progress_open_;
  progress_open_ = false;
  ReleaseAwake(); // on every way out, the page or not
  if (was_open && ports_.close_progress) {
    ports_.close_progress();
  }
}

void ForwardFlow::ReleaseAwake() {
  if (awake_) {
    awake_ = false;
    if (ports_.keep_awake) {
      ports_.keep_awake(false);
    }
  }
}

void ForwardFlow::Tick(const bool chat_visible) {
  if (progress_open_) {
    if (!presenter_.Forwarding()) {
      CloseProgress(); // the run ended or was taken away without telling the page
    } else if (ports_.show_progress) {
      ports_.show_progress(
          MakeForwardProgressView(presenter_.ForwardProgressNow(), ports_.now_ms ? ports_.now_ms() : 0));
    }
  }
  if (chat_visible && !progress_open_ && presenter_.ForwardNoticeDue()) {
    TryNotice();
  }
}

void ForwardFlow::TryNotice() {
  if (presenter_.ForwardNoticePending()) {
    if (!still_sending_said_) {
      still_sending_said_ = true;
      Show(Tr("chat.forward.still_sending")); // the item under way has not answered: the notice waits for it
    }
    return;
  }
  still_sending_said_ = false;
  if (const std::optional<ChatPresenter::ForwardNotice> notice = presenter_.TakeForwardNotice()) {
    Show(NoticeText(*notice));
  }
}

void ForwardFlow::OnProgressCancel() {
  presenter_.CancelForward(ForwardStop::Cancelled); // the page stays, "finishing", until the item under way answers
  Tick(true);
}

void ForwardFlow::OnProgressClose() {
  presenter_.CloseForward(); // runs `done`, which closes the page
}

bool ForwardFlow::OnProgressBack() {
  if (!progress_open_) {
    return false;
  }
  OnProgressCancel();
  return true;
}

void ForwardFlow::OnProgressLayerClosed() {
  if (!progress_open_) {
    return;
  }
  progress_open_ = false; // the layer is already gone
  ReleaseAwake();
  presenter_.CancelForward(ForwardStop::Tapped);
}

void ForwardFlow::OnAppEnteredBackground() {
  presenter_.NoteAppEnteredBackground();
  CloseProgress();
}

void ForwardFlow::OnCallStarted() {
  if (!progress_open_) {
    return;
  }
  presenter_.NoteAppEnteredBackground(); // the same stop: nothing new starts, the item under way is not cut
  CloseProgress();                       // what it did is told when the item under way has answered (Tick)
}

void ForwardFlow::Stop() {
  const bool was_open = picker_open_;
  picker_open_ = false;
  ++picker_turn_;
  single_ = false;
  presenter_.AbortForward();
  if (was_open && ports_.close_picker) {
    ports_.close_picker();
  }
  PickerClosed();
  CloseProgress(); // lock, sign-out, messaging down: the page goes and the screen is let go
  Changed();
}

void ForwardFlow::Leave() {
  const bool was_open = picker_open_;
  picker_open_ = false;
  ++picker_turn_;
  single_ = false;
  presenter_.EndForwardSelection();
  if (was_open && ports_.close_picker) {
    ports_.close_picker();
  }
  PickerClosed();
  Changed();
}

void ForwardFlow::OnThreadGone(const std::string& thread_id) {
  // The presenter has usually forgotten the selection already (it does so when it closes the chat), so this goes by the
  // chat the flow began in and always draws the bars again.
  presenter_.NoteForwardSourceGone(
      thread_id); // a forward with uploads from that chat stops, told at once through the sink
  if (progress_open_ && !presenter_.Forwarding()) {
    CloseProgress();
  }
  if (source_thread_ == thread_id) {
    Leave();
  }
}

void ForwardFlow::OnChatEntered() {
  TryNotice();
}

} // namespace pbr
