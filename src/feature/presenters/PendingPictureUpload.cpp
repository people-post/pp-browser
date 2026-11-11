#include "feature/presenters/PendingPictureUpload.h"

#include <algorithm>
#include <utility>

namespace pbr {

namespace {

using State = PendingPictureState;

/** The row goes: its thumbnail with it. */
PendingPictureStep Removed(const PendingPictureUpload& now) {
  return {PendingPictureUpload{}, now.thumb_path};
}

PendingPictureStep Unchanged(const PendingPictureUpload& now) {
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

PendingPictureUpload StartPendingPicture(const uint64_t serial, const std::string& thread_id,
                                         const std::optional<PicturePixels> upright) {
  PendingPictureUpload upload;
  upload.state = State::Uploading;
  upload.row_id = std::string(kPendingUploadRowPrefix) + std::to_string(serial);
  upload.thread_id = thread_id;
  if (upright && upright->width > 0 && upright->height > 0) {
    upload.pixels = *upright;
    upload.size_known = true;
  }
  return upload;
}

PendingPictureStep OnPictureSizeRead(const PendingPictureUpload& now, const PicturePixels upright) {
  if (now.state != State::Uploading || now.size_known || upright.width <= 0 || upright.height <= 0) {
    return Unchanged(now);
  }
  PendingPictureUpload next = now;
  next.pixels = upright;
  next.size_known = true;
  return {std::move(next), {}};
}

PendingPictureStep OnPictureThumbnail(const PendingPictureUpload& now, const std::string& path, const bool locked) {
  if (now.state != State::Uploading || locked) {
    return {now, path};
  }
  PendingPictureUpload next = now;
  std::string old = std::exchange(next.thumb_path, path);
  return {std::move(next), old == path ? std::string() : old};
}

PendingPictureStep OnPictureProgress(const PendingPictureUpload& now, const int percent) {
  if (now.state != State::Uploading) {
    return Unchanged(now);
  }
  PendingPictureUpload next = now;
  next.percent = std::clamp(std::max(now.percent, percent), 0, 100);
  return {std::move(next), {}};
}

PendingPictureStep OnPictureCancelTapped(const PendingPictureUpload& now) {
  if (now.state != State::Uploading) {
    return Unchanged(now);
  }
  PendingPictureUpload next = now;
  next.state = State::Cancelling;
  return {std::move(next), {}};
}

PendingPictureStep OnPictureSendFinished(const PendingPictureUpload& now, const PictureSendOutcome outcome) {
  switch (now.state) {
    case State::Uploading:
      if (outcome == PictureSendOutcome::FailedStaged) {
        PendingPictureUpload next = now;
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

PendingPictureStep OnPictureRetryTapped(const PendingPictureUpload& now) {
  if (now.state != State::Failed) {
    return Unchanged(now);
  }
  PendingPictureUpload next = now;
  next.state = State::Uploading;
  next.percent = 0;
  return {std::move(next), {}};
}

PendingPictureStep OnPictureDiscardTapped(const PendingPictureUpload& now) {
  return now.state == State::Failed ? Removed(now) : Unchanged(now);
}

PendingPictureStep OnOtherAttachmentStarted(const PendingPictureUpload& now) {
  return now.state == State::Failed ? Removed(now) : Unchanged(now);
}

PendingPictureStep OnAppLocked(const PendingPictureUpload& now) {
  PendingPictureUpload next = now;
  std::string old = std::exchange(next.thumb_path, std::string());
  return {std::move(next), std::move(old)};
}

bool PendingProgressDue(const int drawn, const int percent) {
  return percent > drawn && (percent >= drawn + kPendingProgressStep || percent >= 100);
}

} // namespace pbr
