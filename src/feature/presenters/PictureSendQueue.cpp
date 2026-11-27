#include "feature/presenters/PictureSendQueue.h"

#include "foundation/error/AppError.h"
#include "lib/messaging/AttachmentCache.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <utility>

namespace pbr {

AttachmentSendOutcome ClassifyUploadOutcome(const bool sent, const bool cancelled, const bool staged,
                                            const bool rate_limited) {
  if (sent) {
    return AttachmentSendOutcome::Sent;
  }
  if (cancelled) {
    return AttachmentSendOutcome::Cancelled;
  }
  if (staged) {
    return AttachmentSendOutcome::FailedStaged;
  }
  return rate_limited ? AttachmentSendOutcome::RateLimited : AttachmentSendOutcome::FailedNotStaged;
}

bool IsAttachmentRateLimited(const ::pp::Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Blob &&
         error.code == static_cast<int32_t>(Err::Blob::RateLimited);
}

struct AttachmentSendOnce::State {
  std::function<void(AttachmentSendOutcome)> done;
  AttachmentSendOutcome if_dropped = AttachmentSendOutcome::FailedNotStaged;
  std::atomic<bool> fired{false};

  ~State() {
    if (done && !fired.exchange(true)) {
      done(if_dropped);
    }
  }
};

AttachmentSendOnce::AttachmentSendOnce(std::function<void(AttachmentSendOutcome)> done,
                                       const AttachmentSendOutcome if_dropped) {
  if (done) {
    state_ = std::make_shared<State>();
    state_->done = std::move(done);
    state_->if_dropped = if_dropped;
  }
}

void AttachmentSendOnce::Fire(const AttachmentSendOutcome outcome) const {
  if (state_ && !state_->fired.exchange(true)) {
    state_->done(outcome);
  }
}

bool AttachmentSendOnce::Fired() const {
  return state_ && state_->fired.load();
}

const char* PictureSendItemsTitleKey(const size_t items) {
  return items == 1 ? "picture_send.title_one_file" : "picture_send.title_items";
}

bool PictureOverLimit(const PictureSendItem& item, const bool original, const AttachmentLimits& limits) {
  if (item.kind == PictureSendKind::Video) {
    return !VideoWithinLimit(item.bytes, limits);
  }
  if (item.kind == PictureSendKind::File) {
    return item.bytes > limits.max_file_bytes;
  }
  if (original) {
    return item.bytes > MaxAttachmentSendBytes(limits, AttachmentMimeFromFilename(item.path),
                                               /*image_is_shrunk=*/false);
  }
  return item.pixels > limits.max_image_pixels;
}

PictureSendQueue::PictureSendQueue(std::string thread_id, AttachmentLimits limits, PictureSendPorts ports)
    : thread_id_(std::move(thread_id)), limits_(limits), ports_(std::move(ports)) {}

PictureSendItem* PictureSendQueue::Find(const uint64_t id) {
  auto it = std::find_if(items_.begin(), items_.end(), [id](const PictureSendItem& item) { return item.id == id; });
  return it == items_.end() ? nullptr : &*it;
}

std::optional<uint64_t> PictureSendQueue::Add(std::string path, const PictureSendKind kind, const uint64_t bytes,
                                              const uint64_t pixels, std::string display_name, const bool owned) {
  if (ended_ || items_.size() >= kMaxItems) {
    if (!path.empty()) {
      ports_.release(path);
    }
    return std::nullopt;
  }
  PictureSendItem item;
  item.id = ++next_id_;
  item.path = std::move(path);
  item.kind = kind;
  item.bytes = bytes;
  item.pixels = pixels;
  item.display_name =
      display_name.empty() ? std::filesystem::path(item.path).filename().string() : std::move(display_name);
  item.owned = owned;
  item.too_large = PictureOverLimit(item, original_, limits_);
  const uint64_t id = item.id;
  items_.push_back(std::move(item));
  if (started_) {
    PictureSendItem& added = items_.back();
    if (added.too_large) {
      ReleaseItem(added, PictureItemState::TooLarge);
    }
    Pump();
  }
  return id;
}

void PictureSendQueue::SetArriving(const bool arriving) {
  arriving_ = arriving;
  if (!arriving && started_) {
    Pump(); // the queue may have been waiting only for these
  }
}

void PictureSendQueue::RemovePreviewFiles(PictureSendItem& item) {
  for (std::string* file : {&item.preview_path, &item.thumb_path}) {
    if (!file->empty()) {
      ports_.remove_file(*file);
      file->clear();
    }
  }
}

void PictureSendQueue::ReleaseItem(PictureSendItem& item, const PictureItemState final_state) {
  item.state = final_state;
  RemovePreviewFiles(item);
  if (!item.path.empty()) {
    ports_.release(item.path);
  }
}

bool PictureSendQueue::Remove(const uint64_t id) {
  if (started_ || ended_) {
    return false;
  }
  PictureSendItem* item = Find(id);
  if (!item) {
    return false;
  }
  ReleaseItem(*item, PictureItemState::Cancelled);
  items_.erase(items_.begin() + (item - items_.data()));
  return true;
}

bool PictureSendQueue::SetPreview(const uint64_t id, std::string preview_path, std::string thumb_path) {
  PictureSendItem* item = started_ || ended_ ? nullptr : Find(id);
  if (!item) {
    return false;
  }
  RemovePreviewFiles(*item);
  item->preview_path = std::move(preview_path);
  item->thumb_path = std::move(thumb_path);
  return true;
}

void PictureSendQueue::SetOriginal(const bool original) {
  if (started_ || ended_) {
    return;
  }
  original_ = original;
  for (PictureSendItem& item : items_) {
    item.too_large = PictureOverLimit(item, original_, limits_);
  }
}

void PictureSendQueue::SetText(std::string text) {
  if (!ended_) {
    text_ = std::move(text);
  }
}

uint64_t PictureSendQueue::SelectedBytes() const {
  uint64_t total = 0;
  for (const PictureSendItem& item : items_) {
    total += item.bytes;
  }
  return total;
}

size_t PictureSendQueue::SendableCount() const {
  return static_cast<size_t>(
      std::count_if(items_.begin(), items_.end(), [](const PictureSendItem& item) { return !item.too_large; }));
}

bool PictureSendQueue::Start() {
  if (started_ || ended_ || (SendableCount() == 0 && !arriving_)) {
    return false;
  }
  started_ = true;
  for (PictureSendItem& item : items_) {
    RemovePreviewFiles(item); // the page is gone: its files go now
    if (item.too_large) {
      ReleaseItem(item, PictureItemState::TooLarge);
    }
  }
  Pump();
  return true;
}

void PictureSendQueue::Pump() {
  // send_item may report at once (a refusal): its OnItemFinished then lands here again; one loop, no recursion.
  if (pumping_) {
    pump_again_ = true;
    return;
  }
  pumping_ = true;
  do {
    pump_again_ = false;
    PumpOnce();
  } while (pump_again_);
  pumping_ = false;
}

void PictureSendQueue::PumpOnce() {
  if (!started_ || ended_ || in_flight_ != 0 || retry_at_ms_) {
    return;
  }
  auto next = std::find_if(items_.begin(), items_.end(),
                           [](const PictureSendItem& item) { return item.state == PictureItemState::Waiting; });
  if (next == items_.end()) {
    if (!arriving_) {
      End(PictureSendExit::Finished);
    }
    return;
  }
  if (ports_.offline && ports_.offline()) {
    End(PictureSendExit::Offline); // the rest would only fail: given up, said once
    return;
  }
  next->state = PictureItemState::Sending;
  in_flight_ = next->id;
  const PictureSendItem offered = *next; // a copy: an Add from inside the port may move the items
  ports_.send_item(offered, original_);
}

void PictureSendQueue::OnItemFinished(const uint64_t id, const AttachmentSendOutcome outcome) {
  if (id == 0 || id != in_flight_) {
    return;
  }
  in_flight_ = 0;
  PictureSendItem* item = Find(id);
  if (!item) {
    return;
  }
  if (outcome == AttachmentSendOutcome::RateLimited && !ended_) {
    // Not a failure: nothing was staged and nothing said; the same item goes again once the pace allows.
    item->state = PictureItemState::Waiting;
    retry_at_ms_ = ports_.now_ms() + kRateLimitRetryMs;
    ports_.schedule_tick(kRateLimitRetryMs);
    return;
  }
  PictureItemState final_state = PictureItemState::FailedNotStaged;
  switch (outcome) {
    case AttachmentSendOutcome::Sent:
      final_state = PictureItemState::Sent;
      break;
    case AttachmentSendOutcome::FailedStaged:
      final_state = PictureItemState::FailedStaged;
      break;
    case AttachmentSendOutcome::Cancelled:
      final_state = PictureItemState::Cancelled;
      break;
    case AttachmentSendOutcome::FailedNotStaged:
    case AttachmentSendOutcome::RateLimited: // the queue ended meanwhile: it is not tried again
      final_state = PictureItemState::FailedNotStaged;
      break;
  }
  ReleaseItem(*item, final_state);
  if (ended_) {
    return; // the end that was still in flight: its file is released, nothing else happens
  }
  const bool failed = final_state == PictureItemState::FailedStaged || final_state == PictureItemState::FailedNotStaged;
  if (failed && ports_.offline && ports_.offline()) {
    End(PictureSendExit::Offline);
    return;
  }
  Pump();
}

void PictureSendQueue::Tick() {
  if (ended_ || !retry_at_ms_) {
    return;
  }
  const int64_t now = ports_.now_ms();
  if (now < *retry_at_ms_) {
    ports_.schedule_tick(*retry_at_ms_ - now);
    return;
  }
  retry_at_ms_.reset();
  Pump();
}

void PictureSendQueue::Cancel() {
  if (!started_) {
    Back();
    return;
  }
  End(PictureSendExit::Cancelled);
}

void PictureSendQueue::Back() {
  if (!started_) {
    End(PictureSendExit::Back);
  }
}

void PictureSendQueue::ChatDeleted() {
  End(PictureSendExit::ChatDeleted);
}

void PictureSendQueue::Locked() {
  End(PictureSendExit::Locked);
}

void PictureSendQueue::SignedOut() {
  End(PictureSendExit::SignedOut);
}

void PictureSendQueue::End(const PictureSendExit exit) {
  if (ended_) {
    return;
  }
  ended_ = true;
  retry_at_ms_.reset();
  PictureSendReport report;
  report.exit = exit;
  for (PictureSendItem& item : items_) {
    if (item.state == PictureItemState::Waiting) {
      ReleaseItem(item, PictureItemState::Cancelled);
    } else {
      RemovePreviewFiles(item);
    }
    switch (item.state) {
      case PictureItemState::Sending:
        ++report.in_flight; // released when its end comes
        break;
      case PictureItemState::Sent:
        ++report.sent;
        break;
      case PictureItemState::FailedStaged:
        ++report.failed_staged;
        break;
      case PictureItemState::FailedNotStaged:
        ++report.failed_not_staged;
        break;
      case PictureItemState::TooLarge:
        ++report.too_large;
        break;
      case PictureItemState::Waiting:
      case PictureItemState::Cancelled:
        ++report.cancelled;
        break;
    }
  }
  if (text_.empty()) {
    report.text = PictureTextFate::None;
  } else if (exit == PictureSendExit::Finished) {
    // Only now, after the last picture: never before one of them, and never lost when it cannot go.
    report.text = ports_.send_text(thread_id_, text_) ? PictureTextFate::Sent : PictureTextFate::PutBack;
  } else if (exit == PictureSendExit::Cancelled || exit == PictureSendExit::Offline || exit == PictureSendExit::Back) {
    report.text = PictureTextFate::PutBack;
  } else {
    report.text = PictureTextFate::Dropped; // the chat or the session is gone with it
  }
  if (report.text == PictureTextFate::PutBack) {
    ports_.put_back_text(thread_id_, text_);
  }
  if (ports_.done) {
    ports_.done(report);
  }
}

PictureSendProgress PictureSendQueue::Progress() const {
  PictureSendProgress progress;
  size_t finished = 0;
  for (const PictureSendItem& item : items_) {
    if (item.too_large) {
      continue;
    }
    ++progress.of;
    if (item.state != PictureItemState::Waiting && item.state != PictureItemState::Sending) {
      ++finished;
    }
  }
  progress.number = started_ && !ended_ ? std::min(finished + 1, progress.of) : finished;
  progress.paced = retry_at_ms_.has_value();
  return progress;
}

} // namespace pbr
