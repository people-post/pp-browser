#pragma once

#include "domain/attachments/StoredAttachments.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Me → Storage and its "Manage storage" pages (DEV/PLAN-storage-manager.md): pure grouping, sorting and sizing of
 * the attachments this device holds. Bytes are always the stored bytes (StoredAttachmentItem::bytes), never what
 * the sender declared. No I/O except DirectoryByteSize.
 */

/** "Files larger than 5 MB" on the manage page. */
inline constexpr uint64_t kStorageLargeItemBytes = 5ull * 1024 * 1024;
/** Grid cells (and file rows) shown before "Show more". */
inline constexpr size_t kStoragePageSize = 60;

enum class StoredKind { Picture, Video, File };

/** An image mime → picture, a video mime → video, anything else → file. */
StoredKind StoredKindOfMime(const std::string& mime);
/** "picture" / "video" / "file" (the RML ids). */
const char* StoredKindId(StoredKind kind);
std::optional<StoredKind> StoredKindFromId(const std::string& id);

/** "512 B", "8.4 KB", "42 MB", "1.2 GB". */
std::string FormatByteSize(uint64_t bytes);
/** Bytes of every regular file under `root` (0 when missing). Blocking. */
uint64_t DirectoryByteSize(const std::filesystem::path& root);

/** Counts and bytes per kind, each content hash counted once. */
struct StorageKindTotals {
  size_t pictures = 0;
  size_t videos = 0;
  size_t files = 0;
  uint64_t picture_bytes = 0;
  uint64_t video_bytes = 0;
  uint64_t file_bytes = 0;

  size_t Count() const { return pictures + videos + files; }
  uint64_t Bytes() const { return picture_bytes + video_bytes + file_bytes; }
  size_t CountOf(StoredKind kind) const;
};

/** Totals of `items`, each content hash once (the first item seen for it decides its kind). */
StorageKindTotals StorageTotalsOf(const std::vector<StoredAttachmentItem>& items);

struct StorageChatSummary {
  std::string thread_id;
  StorageKindTotals totals;  // this chat's items, each hash once
};

struct StorageOverview {
  StorageKindTotals totals;                 // every chat, each hash once
  std::vector<StorageChatSummary> chats;    // chats holding anything, most bytes first
  std::vector<StoredAttachmentItem> large;  // > kStorageLargeItemBytes, one per hash (newest), most bytes first
  uint64_t large_bytes = 0;
};

StorageOverview BuildStorageOverview(const std::vector<StoredAttachmentItem>& items);

/** One chat's items of one kind, newest first. */
std::vector<StoredAttachmentItem> StorageItemsOfChat(const std::vector<StoredAttachmentItem>& items,
                                                     const std::string& thread_id, StoredKind kind);
/** The large list's items of one kind (already one per hash), most bytes first. */
std::vector<StoredAttachmentItem> StorageItemsOfKind(const std::vector<StoredAttachmentItem>& items, StoredKind kind);

/**
 * What a delete of `selected` names: on a chat's page its own (chat, hash) pairs; from the large list
 * (`every_chat`) every chat that holds each selected hash.
 */
std::vector<StoredAttachmentRef> StorageDeleteRefs(const std::vector<StoredAttachmentItem>& all,
                                                   const std::vector<StoredAttachmentItem>& selected, bool every_chat);

/** Bytes the selected items take, each hash once. */
uint64_t StorageSelectedBytes(const std::vector<StoredAttachmentItem>& selected);

/** What the delete confirmation says beyond the count. */
struct StorageDeleteFacts {
  size_t count = 0;
  /** Selected hashes no chat has a server copy of: deleting them cannot be undone. */
  size_t only_here = 0;
  /** Selected hashes another chat (not deleted now) also holds: their space is not freed. */
  size_t shared_elsewhere = 0;
};

StorageDeleteFacts StorageDeleteFactsOf(const std::vector<StoredAttachmentItem>& all,
                                        const std::vector<StoredAttachmentItem>& selected,
                                        const std::vector<StoredAttachmentRef>& refs);

/** Me → Storage's usage bar: the profile's bytes split into what is known, the rest "other". */
struct StorageUsage {
  uint64_t total = 0;
  uint64_t pictures = 0;
  uint64_t videos = 0;
  uint64_t files = 0;
  uint64_t chats = 0;  // the message databases
  uint64_t other = 0;  // logs, avatars, AI data, caches
};

/**
 * Splits `profile_bytes`: attachments by kind, then the message databases (never more than what is left), the
 * remainder "other". A total smaller than the attachments (a race between the two counts) grows to fit.
 */
StorageUsage ComputeStorageUsage(uint64_t profile_bytes, const StorageKindTotals& attachments,
                                 uint64_t message_store_bytes);

struct StorageBarSegment {
  std::string kind;  // picture | video | file | chats | other
  uint64_t bytes = 0;
  float percent = 0.f;
};

/** The non-empty segments, in order, as percents of their sum; `with_chats` adds chats and other. */
std::vector<StorageBarSegment> StorageBarSegments(const StorageUsage& usage, bool with_chats);

/**
 * At most one recompute of the manage lists per interval after attachment changes, and never two at once:
 * a change while one runs or one is scheduled only marks the lists stale.
 */
class StorageRecomputeGate {
public:
  enum class Step { Nothing, RunNow, RunLater };
  struct Decision {
    Step step = Step::Nothing;
    int64_t delay_ms = 0;
  };

  explicit StorageRecomputeGate(int64_t min_interval_ms = 2000) : interval_ms_(min_interval_ms) {}

  /** An attachment changed. RunNow: start one (and call OnStarted); RunLater: call OnTimer after delay_ms. */
  Decision OnChange(int64_t now_ms);
  void OnStarted(int64_t now_ms);
  /** The recompute finished: a change that came meanwhile asks for the next one. */
  Decision OnFinished(int64_t now_ms);
  /** The scheduled time came. */
  Decision OnTimer(int64_t now_ms);
  /** The page closed: nothing runs or waits any more (a timer still pending finds nothing to do). */
  void Reset();

private:
  Decision Next(int64_t now_ms);

  int64_t interval_ms_;
  std::optional<int64_t> last_start_ms_;
  bool running_ = false;
  bool scheduled_ = false;
  bool stale_ = false;
};

} // namespace pbr
