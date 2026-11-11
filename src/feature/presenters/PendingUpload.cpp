#include "feature/presenters/PendingUpload.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace pbr {

namespace {

using State = PendingUploadState;

/** The row goes: its thumbnail with it. */
PendingUploadStep Removed(const PendingUpload& now) {
  return {PendingUpload{}, now.thumb_path};
}

PendingUploadStep Unchanged(const PendingUpload& now) {
  return {now, {}};
}

} // namespace

bool IsPendingUploadRowId(const std::string_view message_id) {
  return message_id.substr(0, kPendingUploadRowPrefix.size()) == kPendingUploadRowPrefix;
}

PicturePixels UprightPixels(const int width, const int height, const int exif_orientation) {
  if (exif_orientation >= 5 && exif_orientation <= 8) {
    return {height, width};
  }
  return {width, height};
}

PendingUpload StartPendingUpload(const uint64_t serial, const std::string& thread_id,
                                 const std::optional<PicturePixels> upright, const PendingUploadKind kind) {
  PendingUpload upload;
  upload.state = State::Uploading;
  upload.kind = kind;
  upload.row_id = std::string(kPendingUploadRowPrefix) + std::to_string(serial);
  upload.thread_id = thread_id;
  if (upright && upright->width > 0 && upright->height > 0) {
    upload.pixels = *upright;
    upload.size_known = true;
  }
  return upload;
}

PendingUploadStep OnUploadSizeRead(const PendingUpload& now, const PicturePixels upright) {
  if (now.state != State::Uploading || upright.width <= 0 || upright.height <= 0) {
    return Unchanged(now);
  }
  if (now.size_known) {
    // Same proportions within 2% (the thumbnail's sides are rounded): the head's size stays.
    const int64_t head = static_cast<int64_t>(now.pixels.width) * upright.height;
    const int64_t thumb = static_cast<int64_t>(upright.width) * now.pixels.height;
    if (std::llabs(head - thumb) * 50 <= std::max(head, thumb)) {
      return Unchanged(now);
    }
  }
  PendingUpload next = now;
  next.pixels = upright;
  next.size_known = true;
  return {std::move(next), {}};
}

PendingUploadStep OnUploadThumbnail(const PendingUpload& now, const std::string& path, const bool locked) {
  if (now.state != State::Uploading || locked) {
    return {now, path};
  }
  PendingUpload next = now;
  std::string old = std::exchange(next.thumb_path, path);
  return {std::move(next), old == path ? std::string() : old};
}

PendingUploadStep OnUploadDetails(const PendingUpload& now, const std::string& title, const std::string& size_text) {
  if (!now.Shows()) {
    return Unchanged(now);
  }
  PendingUpload next = now;
  if (!title.empty()) {
    next.title = title;
  }
  next.size_text = size_text;
  return {std::move(next), {}};
}

PendingUploadStep OnUploadProgress(const PendingUpload& now, const int percent) {
  if (now.state != State::Uploading) {
    return Unchanged(now);
  }
  PendingUpload next = now;
  next.percent = std::clamp(std::max(now.percent, percent), 0, 100);
  return {std::move(next), {}};
}

PendingUploadStep OnUploadCancelTapped(const PendingUpload& now) {
  if (now.state != State::Uploading) {
    return Unchanged(now);
  }
  PendingUpload next = now;
  next.state = State::Cancelling;
  return {std::move(next), {}};
}

PendingUploadStep OnUploadSendFinished(const PendingUpload& now, const UploadSendOutcome outcome) {
  switch (now.state) {
    case State::Uploading:
      if (outcome == UploadSendOutcome::FailedStaged) {
        PendingUpload next = now;
        next.state = State::Failed;
        return {std::move(next), {}};
      }
      return Removed(now);
    case State::Cancelling:
      return Removed(now); // sent anyway (the controller says it was too late), cancelled, or failed: the row goes
    case State::None:
    case State::Failed:
      break;
  }
  return Unchanged(now);
}

PendingUploadStep OnUploadRetryTapped(const PendingUpload& now) {
  if (now.state != State::Failed) {
    return Unchanged(now);
  }
  PendingUpload next = now;
  next.state = State::Uploading;
  next.percent = 0;
  return {std::move(next), {}};
}

PendingUploadStep OnUploadDiscardTapped(const PendingUpload& now) {
  return now.state == State::Failed ? Removed(now) : Unchanged(now);
}

PendingUploadStep OnOtherAttachmentStarted(const PendingUpload& now) {
  return now.state == State::Failed ? Removed(now) : Unchanged(now);
}

PendingUploadStep OnAppLocked(const PendingUpload& now) {
  PendingUpload next = now;
  std::string old = std::exchange(next.thumb_path, std::string());
  return {std::move(next), std::move(old)};
}

bool FailedVoiceTakesTheTap(const bool voice_failed, const std::string& voice_thread, const std::string& active_thread,
                            const bool picture_bubble_here) {
  return voice_failed && !active_thread.empty() && voice_thread == active_thread && !picture_bubble_here;
}

bool PendingProgressDue(const int drawn, const int percent) {
  return percent > drawn && (percent >= drawn + kPendingProgressStep || percent >= 100);
}

const PendingUpload* PendingUploadQueue::Find(const std::string& id) const {
  for (const PendingUpload& item : items_) {
    if (item.row_id == id) {
      return &item;
    }
  }
  return nullptr;
}

const PendingUpload* PendingUploadQueue::InFlightIn(const std::string& thread_id) const {
  for (const PendingUpload& item : items_) {
    if (item.ShowsIn(thread_id) && (item.state == State::Uploading || item.state == State::Cancelling)) {
      return &item;
    }
  }
  return nullptr;
}

bool PendingUploadQueue::ShowsIn(const std::string& thread_id) const {
  for (const PendingUpload& item : items_) {
    if (item.ShowsIn(thread_id)) {
      return true;
    }
  }
  return false;
}

void PendingUploadQueue::Add(PendingUpload item) {
  if (item.Shows()) {
    items_.push_back(std::move(item));
  }
}

std::vector<std::string> PendingUploadQueue::Clear() {
  std::vector<std::string> files;
  for (const PendingUpload& item : items_) {
    if (!item.thumb_path.empty()) {
      files.push_back(item.thumb_path);
    }
  }
  items_.clear();
  return files;
}

} // namespace pbr
