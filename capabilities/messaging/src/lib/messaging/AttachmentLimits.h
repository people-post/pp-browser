#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pbr {

/** One user-facing megabyte: decimal, the same unit the on-screen limits are written in. */
inline constexpr uint64_t kAttachmentMegabyte = 1000ULL * 1000ULL;

/**
 * The one size limit today for video, files, audio, original pictures and receiving (over 16 MiB of ciphertext
 * goes to the relay in parts). Later it is raised by account level: only the source of `AttachmentLimits` changes.
 */
inline constexpr uint64_t kLargeAttachmentMaxBytes = 50ULL * kAttachmentMegabyte;
inline constexpr uint64_t kDefaultAttachmentSendMaxBytes = kLargeAttachmentMaxBytes;
inline constexpr uint64_t kDefaultAttachmentReceiveMaxBytes = kLargeAttachmentMaxBytes;

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
  /**
   * The longest clip the system camera is asked to record (iOS `videoMaximumDuration`, Android
   * `EXTRA_DURATION_LIMIT`). Follows the 50 MB `max_video_bytes`: medium-quality capture is about 0.5 MB/s, so a
   * minute stays well under it (a clip over the limit is still refused by `VideoWithinLimit`).
   */
  int video_max_seconds = 60;
  /** A send of at least this many bytes tells the person to stay in the app until it is done. */
  uint64_t stay_hint_bytes = 10ULL * kAttachmentMegabyte;
  /** Server space warnings, in percent of the space in use. */
  int warn_percent_low = 80;
  int warn_percent_high = 90;

  /** A picture larger than this many pixels is not decoded (it would exhaust memory). */
  uint64_t max_image_pixels = 64ULL * 1000ULL * 1000ULL;

  /** Sending: attachments one person may send per minute (all types together). */
  int send_max_per_minute = 10;
  /** Receiving: a size at or below which "smart" downloads start by themselves. */
  uint64_t auto_download_max_bytes = 4ULL * kAttachmentMegabyte;
  /** Receiving: per sender and minute, how many and how many bytes may start downloading by themselves. */
  int auto_download_max_count_per_minute = 10;
  uint64_t auto_download_max_bytes_per_minute = 20ULL * kAttachmentMegabyte;
  /** The same byte limit when "download everything" is chosen and the current network allows it. */
  uint64_t auto_download_all_max_bytes_per_minute = 80ULL * kAttachmentMegabyte;
  /** Receiving: once the attachment store is this big no large file (above the direct size) starts by itself. */
  uint64_t auto_download_disk_cap_bytes = 1000ULL * kAttachmentMegabyte;
  /** ...and once it is this big no small one does either (large files do not stop the small ones). */
  uint64_t auto_download_small_disk_cap_bytes = 1500ULL * kAttachmentMegabyte;
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

/** A recorded video of `bytes` may be sent: the camera does not always honour the size limit it was given. */
bool VideoWithinLimit(uint64_t bytes, const AttachmentLimits& limits);

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
  /**
   * True and recorded when this download may start now; false when the sender is over a limit: the count is
   * at `max_count`, or the bytes already let through this minute plus this one are above `max_bytes`.
   */
  bool TryAcquire(const std::string& sender, uint64_t bytes, int64_t now_ms, int max_count, uint64_t max_bytes);
  /** Gives back one recorded download of `bytes` (it did not start after all); nothing when it is out of the window. */
  void Refund(const std::string& sender, uint64_t bytes);

private:
  struct Entry {
    int64_t at_ms = 0;
    uint64_t bytes = 0;
  };
  std::mutex mutex_;
  std::unordered_map<std::string, std::deque<Entry>> by_sender_;
};

/** The files relay's space (`/files/list` usage): attachments, the space that fills. */
struct BlobSpaceUse {
  uint64_t bytes_used = 0;
  uint64_t quota_bytes = 0;
};

/** Percent of the files relay's quota in use; 0 when the quota is not known. */
int BlobSpaceUsedPercent(const BlobSpaceUse& use);

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
