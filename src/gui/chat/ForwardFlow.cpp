#include "gui/chat/ForwardFlow.h"

#include "foundation/i18n/LocalizationService.h"

#include <algorithm>
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

std::string ForwardResultText(const ForwardResult& result) {
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
    : presenter_(presenter), ports_(std::move(ports)) {}

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

void ForwardFlow::Begin(const std::string& message_id) {
  if (presenter_.Forwarding()) {
    Show(Tr("chat.send.still_trying")); // the earlier forward is still being queued: the reason, not "cannot"
    return;
  }
  if (!presenter_.BeginForwardSelection(message_id)) {
    Show(Tr("chat.forward.cannot"));
    return;
  }
  source_thread_ = presenter_.ForwardSourceThread();
  Changed();
}

void ForwardFlow::Tap(const std::string& message_id) {
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
  presenter_.EndForwardSelection(); // only the choosing: a forward already being sent goes on
  Changed();
}

void ForwardFlow::Next() {
  if (!CanNext() || !ports_.open_picker) {
    return;
  }
  PeoplePickerChooseOptions options;
  options.title = Tr("chat.forward.pick_title");
  options.send_label = Tr("picture_send.send_one"); // the app's plain "Send"
  options.preview = presenter_.ForwardPreview(
      [](const size_t count) { return Tr("chat.forward.preview_count", {{"n", std::to_string(count)}}); });
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
        if (turn == picker_turn_) { // closed without choosing: the messages stay ticked
          picker_open_ = false;
          Changed();
        }
      });
}

void ForwardFlow::OnChosen(std::vector<ForwardTarget> chosen) {
  picker_open_ = false;
  ++picker_turn_;
  if (!Selecting() || chosen.empty()) {
    Changed();
    return;
  }
  presenter_.ForwardSelectedTo(std::move(chosen),
                               [this](const ForwardResult& result) { Show(ForwardResultText(result)); });
  Changed(); // choosing is over at once; the pill follows when everything is queued
}

void ForwardFlow::Stop() {
  const bool was_open = picker_open_;
  picker_open_ = false;
  ++picker_turn_;
  presenter_.AbortForward();
  if (was_open && ports_.close_picker) {
    ports_.close_picker();
  }
  Changed();
}

void ForwardFlow::Leave() {
  const bool was_open = picker_open_;
  picker_open_ = false;
  ++picker_turn_;
  presenter_.EndForwardSelection();
  if (was_open && ports_.close_picker) {
    ports_.close_picker();
  }
  Changed();
}

void ForwardFlow::OnThreadGone(const std::string& thread_id) {
  // The presenter has usually forgotten the selection already (it does so when it closes the chat), so this goes by the
  // chat the flow began in and always draws the bars again.
  if (source_thread_ == thread_id) {
    Leave();
  }
}

void ForwardFlow::OnChatEntered() {
  if (presenter_.TakeForwardInterrupted()) {
    Show(Tr("chat.forward.interrupted"));
  }
}

} // namespace pbr
