#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pbr {

/** One user-facing megabyte: decimal, the same unit the on-screen limits are written in. */
inline constexpr uint64_t kAttachmentMegabyte = 1000ULL * 1000ULL;

/** The size the larger tiers are planned for (video, files, audio, original pictures, receiving). */
inline constexpr uint64_t kLargeAttachmentMaxBytes = 100ULL * kAttachmentMegabyte;
/**
 * What ships today. Streaming upload / download (step 2) is not in yet, so send and receive stay at the
 * small size; step 2 only changes these two to `kLargeAttachmentMaxBytes`.
 */
inline constexpr uint64_t kDefaultAttachmentSendMaxBytes = 4ULL * kAttachmentMegabyte;
inline constexpr uint64_t kDefaultAttachmentReceiveMaxBytes = 4ULL * kAttachmentMegabyte;

/**
 * Every number that limits what a person may send, receive or keep, behind one value so a later tier
 * (a member level) swaps the source and no caller changes. Sizes are decimal bytes. Read it through
 * `ConversationsFacade::CurrentAttachmentLimits()`.
 */
struct AttachmentLimits {
  /** A picture after the automatic shrink. */
  uint64_t max_image_bytes = 4ULL * kAttachmentMegabyte;
  /** A picture sent as it is ("original") and a GIF. */
  uint64_t max_original_image_bytes = kDefaultAttachmentSendMaxBytes;
  uint64_t max_file_bytes = kDefaultAttachmentSendMaxBytes;
  uint64_t max_video_bytes = kDefaultAttachmentSendMaxBytes;
  uint64_t max_audio_bytes = kDefaultAttachmentSendMaxBytes;
  /** A received attachment whose declared size is above this is never downloaded. */
  uint64_t max_receive_bytes = kDefaultAttachmentReceiveMaxBytes;
  int voice_max_seconds = 120;
  /** Server space warnings, in percent of the space in use. */
  int warn_percent_low = 80;
  int warn_percent_high = 90;
  /** How many small files the server keeps per person (the list answer carries no such number). */
  int small_max_objects = 100;

  /** A picture larger than this many pixels is not decoded (it would exhaust memory). */
  uint64_t max_image_pixels = 64ULL * 1000ULL * 1000ULL;

  /** Sending: attachments one person may send per minute (all types together). */
  int send_max_per_minute = 10;
  /** Receiving: a size at or below which "smart" downloads start by themselves. */
  uint64_t auto_download_max_bytes = 4ULL * kAttachmentMegabyte;
  /** Receiving: per sender and minute, how many and how many bytes may start downloading by themselves. */
  int auto_download_max_count_per_minute = 10;
  uint64_t auto_download_max_bytes_per_minute = 20ULL * kAttachmentMegabyte;
  /** Receiving: once the attachment store is this big nothing starts by itself any more. */
  uint64_t auto_download_disk_cap_bytes = 1000ULL * kAttachmentMegabyte;
};

AttachmentLimits DefaultAttachmentLimits();

/** What a file is for the limits: decided from its mime type. */
enum class AttachmentKind { Image, File, Video, Audio };
AttachmentKind AttachmentKindOfMime(const std::string& mime);

/**
 * The most bytes this attachment may have when sent. `image_is_shrunk`: the picture went through the
 * automatic shrink (a GIF and an "original" did not).
 */
uint64_t MaxAttachmentSendBytes(const AttachmentLimits& limits, const std::string& mime, bool image_is_shrunk);

/** "4 MB", "100 MB": a limit as it is written to people. */
std::string FormatAttachmentLimit(uint64_t bytes);

/**
 * Sliding one-minute window of the attachments this device sent. Stops a slip of the finger and an
 * ordinary user; it cannot stop a changed client (the receiver's own limits do that).
 */
class AttachmentSendLimiter {
public:
  /** True and recorded when another send is allowed at `now_ms`; false when `max_per_minute` are already in the window. */
  bool TryAcquire(int64_t now_ms, int max_per_minute);

private:
  std::mutex mutex_;
  std::deque<int64_t> sent_ms_;
};

/**
 * What a receiver lets one sender start downloading by itself: a count and a byte total per minute,
 * counted in memory by sender (in a group, the sender of the message). Files over the limit stay a
 * row with "Download".
 */
class AutoDownloadBudget {
public:
  /** True and recorded when this download may start now; false when the sender is over a limit. */
  bool TryAcquire(const std::string& sender, uint64_t bytes, int64_t now_ms, int max_count, uint64_t max_bytes);

private:
  struct Entry {
    int64_t at_ms = 0;
    uint64_t bytes = 0;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, std::deque<Entry>> by_sender_;
};

/** Which share of the server space is in use, and which warning that crosses. */
struct BlobSpaceUse {
  uint64_t small_count = 0;
  uint64_t large_bytes = 0;
  uint64_t large_included_bytes = 0;
};

/**
 * Percent of the space used: the fuller of the large tier (bytes of what the plan includes) and the
 * small tier (files of `small_max_objects`). 0 when neither denominator is known.
 */
int BlobSpaceUsedPercent(const BlobSpaceUse& use, const AttachmentLimits& limits);

/** The warning level (`warn_percent_low` / `warn_percent_high`) a usage reaches; 0 below the first. */
int BlobSpaceWarnLevel(int used_percent, const AttachmentLimits& limits);

/**
 * When to look at the server space and what to say: at most one look a day (and one after every "space
 * full" answer), and each warning level only once while the app runs.
 */
class BlobSpaceMonitor {
public:
  /** True when the space should be read now; records the look. `force`: after a "space full" answer. */
  bool ShouldCheck(int64_t now_ms, bool force);
  /** The level (80 / 90) to tell the person about for this usage, or 0 when it was already said or is below. */
  int NextWarning(int used_percent, const AttachmentLimits& limits);

private:
  std::mutex mutex_;
  int64_t checked_ms_ = 0;
  int said_level_ = 0;
};

} // namespace pbr
