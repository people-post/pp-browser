#include "lib/messaging/AttachmentLimits.h"

#include <algorithm>

namespace pbr {

namespace {
constexpr int64_t kWindowMs = 60 * 1000;
} // namespace

AttachmentLimits DefaultAttachmentLimits() { return AttachmentLimits{}; }

AttachmentKind AttachmentKindOfMime(const std::string& mime) {
  if (mime.rfind("image/", 0) == 0) {
    return AttachmentKind::Image;
  }
  if (mime.rfind("video/", 0) == 0) {
    return AttachmentKind::Video;
  }
  if (mime.rfind("audio/", 0) == 0) {
    return AttachmentKind::Audio;
  }
  return AttachmentKind::File;
}

bool VideoWithinLimit(const uint64_t bytes, const AttachmentLimits& limits) { return bytes <= limits.max_video_bytes; }

uint64_t MaxAttachmentSendBytes(const AttachmentLimits& limits, const std::string& mime, const bool image_is_shrunk) {
  switch (AttachmentKindOfMime(mime)) {
  case AttachmentKind::Image:
    return image_is_shrunk ? limits.max_image_bytes : limits.max_original_image_bytes;
  case AttachmentKind::Video:
    return limits.max_video_bytes;
  case AttachmentKind::Audio:
    return limits.max_audio_bytes;
  case AttachmentKind::File:
    break;
  }
  return limits.max_file_bytes;
}

std::string FormatAttachmentLimit(const uint64_t bytes) {
  return std::to_string(std::max<uint64_t>(1, bytes / kAttachmentMegabyte)) + " MB";
}

bool AttachmentSendLimiter::TryAcquire(const int64_t now_ms, const int max_per_minute) {
  std::lock_guard lock(mutex_);
  while (!sent_ms_.empty() && now_ms - sent_ms_.front() >= kWindowMs) {
    sent_ms_.pop_front();
  }
  if (static_cast<int>(sent_ms_.size()) >= max_per_minute) {
    return false;
  }
  sent_ms_.push_back(now_ms);
  return true;
}

bool AutoDownloadBudget::TryAcquire(const std::string& sender, const uint64_t bytes, const int64_t now_ms,
                                    const int max_count, const uint64_t max_bytes) {
  std::lock_guard lock(mutex_);
  auto& entries = by_sender_[sender];
  while (!entries.empty() && now_ms - entries.front().at_ms >= kWindowMs) {
    entries.pop_front();
  }
  uint64_t used_bytes = 0;
  for (const Entry& entry : entries) {
    used_bytes += entry.bytes;
  }
  if (static_cast<int>(entries.size()) >= max_count || used_bytes + bytes > max_bytes) {
    return false;
  }
  entries.push_back(Entry{.at_ms = now_ms, .bytes = bytes});
  return true;
}

int BlobSpaceUsedPercent(const BlobSpaceUse& use, const AttachmentLimits& limits) {
  int percent = 0;
  if (use.large_included_bytes > 0) {
    percent = std::max<int>(percent, static_cast<int>(use.large_bytes * 100 / use.large_included_bytes));
  }
  if (limits.small_max_objects > 0) {
    percent = std::max<int>(percent, static_cast<int>(use.small_count * 100 / static_cast<uint64_t>(limits.small_max_objects)));
  }
  return percent;
}

int BlobSpaceWarnLevel(const int used_percent, const AttachmentLimits& limits) {
  if (used_percent >= limits.warn_percent_high) {
    return limits.warn_percent_high;
  }
  if (used_percent >= limits.warn_percent_low) {
    return limits.warn_percent_low;
  }
  return 0;
}

bool BlobSpaceMonitor::ShouldCheck(const int64_t now_ms, const bool force) {
  constexpr int64_t kDayMs = 24LL * 60LL * 60LL * 1000LL;
  std::lock_guard lock(mutex_);
  if (!force && checked_ms_ != 0 && now_ms - checked_ms_ < kDayMs) {
    return false;
  }
  checked_ms_ = now_ms;
  return true;
}

int BlobSpaceMonitor::NextWarning(const int used_percent, const AttachmentLimits& limits) {
  const int level = BlobSpaceWarnLevel(used_percent, limits);
  std::lock_guard lock(mutex_);
  if (level == 0 || level <= said_level_) {
    return 0;
  }
  said_level_ = level;
  return level;
}

} // namespace pbr
