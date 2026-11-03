#include "feature/settings/StorageManagerModel.h"

#include "lib/messaging/AttachmentCache.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace pbr {

namespace {

std::string HashKey(const std::vector<uint8_t>& hash) {
  return std::string(hash.begin(), hash.end());
}

void AddToTotals(StorageKindTotals& totals, const StoredAttachmentItem& item) {
  switch (StoredKindOfMime(item.mime)) {
  case StoredKind::Picture:
    ++totals.pictures;
    totals.picture_bytes += item.bytes;
    return;
  case StoredKind::Video:
    ++totals.videos;
    totals.video_bytes += item.bytes;
    return;
  case StoredKind::File:
    ++totals.files;
    totals.file_bytes += item.bytes;
    return;
  }
}

bool NewerFirst(const StoredAttachmentItem& a, const StoredAttachmentItem& b) {
  if (a.timestamp_ms != b.timestamp_ms) {
    return a.timestamp_ms > b.timestamp_ms;
  }
  return a.message_id < b.message_id;
}

} // namespace

StoredKind StoredKindOfMime(const std::string& mime) {
  if (IsAttachmentImageMime(mime)) {
    return StoredKind::Picture;
  }
  if (IsAttachmentVideoMime(mime)) {
    return StoredKind::Video;
  }
  return StoredKind::File;
}

const char* StoredKindId(const StoredKind kind) {
  switch (kind) {
  case StoredKind::Picture:
    return "picture";
  case StoredKind::Video:
    return "video";
  case StoredKind::File:
    return "file";
  }
  return "file";
}

std::optional<StoredKind> StoredKindFromId(const std::string& id) {
  if (id == "picture") {
    return StoredKind::Picture;
  }
  if (id == "video") {
    return StoredKind::Video;
  }
  if (id == "file") {
    return StoredKind::File;
  }
  return std::nullopt;
}

std::string FormatByteSize(const uint64_t bytes) {
  constexpr double kKiB = 1024.0;
  constexpr double kMiB = kKiB * 1024.0;
  constexpr double kGiB = kMiB * 1024.0;

  std::ostringstream out;
  out.setf(std::ios::fixed);
  if (bytes < static_cast<uint64_t>(kKiB)) {
    out << bytes << " B";
  } else if (bytes < static_cast<uint64_t>(kMiB)) {
    out.precision(bytes < static_cast<uint64_t>(10 * kKiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kKiB) << " KB";
  } else if (bytes < static_cast<uint64_t>(kGiB)) {
    out.precision(bytes < static_cast<uint64_t>(10 * kMiB) ? 1 : 0);
    out << (static_cast<double>(bytes) / kMiB) << " MB";
  } else {
    out.precision(1);
    out << (static_cast<double>(bytes) / kGiB) << " GB";
  }
  return out.str();
}

uint64_t DirectoryByteSize(const std::filesystem::path& root) {
  std::error_code ec;
  if (!std::filesystem::exists(root, ec) || ec) {
    return 0;
  }

  uint64_t total = 0;
  const auto options = std::filesystem::directory_options::skip_permission_denied;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, options, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const uint64_t size = entry.file_size(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    total += size;
  }
  return total;
}

size_t StorageKindTotals::CountOf(const StoredKind kind) const {
  switch (kind) {
  case StoredKind::Picture:
    return pictures;
  case StoredKind::Video:
    return videos;
  case StoredKind::File:
    return files;
  }
  return 0;
}

StorageKindTotals StorageTotalsOf(const std::vector<StoredAttachmentItem>& items) {
  StorageKindTotals totals;
  std::unordered_set<std::string> seen;
  for (const StoredAttachmentItem& item : items) {
    if (seen.insert(HashKey(item.content_hash)).second) {
      AddToTotals(totals, item);
    }
  }
  return totals;
}

StorageOverview BuildStorageOverview(const std::vector<StoredAttachmentItem>& items) {
  StorageOverview overview;
  overview.totals = StorageTotalsOf(items);

  // Per chat, each hash once (the list is one per (chat, hash) already; a defensive dedupe costs nothing).
  std::map<std::string, std::vector<StoredAttachmentItem>> by_chat;
  for (const StoredAttachmentItem& item : items) {
    by_chat[item.thread_id].push_back(item);
  }
  for (const auto& [thread_id, chat_items] : by_chat) {
    StorageChatSummary summary{.thread_id = thread_id, .totals = StorageTotalsOf(chat_items)};
    if (summary.totals.Count() > 0) {
      overview.chats.push_back(std::move(summary));
    }
  }
  std::stable_sort(overview.chats.begin(), overview.chats.end(),
                   [](const StorageChatSummary& a, const StorageChatSummary& b) {
                     return a.totals.Bytes() > b.totals.Bytes();
                   });

  // Larger than 5 MB across chats: one entry per hash (its newest message), most bytes first.
  std::unordered_map<std::string, size_t> large_index;
  for (const StoredAttachmentItem& item : items) {
    if (item.bytes <= kStorageLargeItemBytes) {
      continue;
    }
    const std::string key = HashKey(item.content_hash);
    const auto found = large_index.find(key);
    if (found == large_index.end()) {
      large_index.emplace(key, overview.large.size());
      overview.large.push_back(item);
      overview.large_bytes += item.bytes;
    } else if (NewerFirst(item, overview.large[found->second])) {
      overview.large[found->second] = item;
    }
  }
  std::stable_sort(overview.large.begin(), overview.large.end(),
                   [](const StoredAttachmentItem& a, const StoredAttachmentItem& b) {
                     if (a.bytes != b.bytes) {
                       return a.bytes > b.bytes;
                     }
                     return NewerFirst(a, b);
                   });
  return overview;
}

std::vector<StoredAttachmentItem> StorageItemsOfChat(const std::vector<StoredAttachmentItem>& items,
                                                     const std::string& thread_id, const StoredKind kind) {
  std::vector<StoredAttachmentItem> out;
  std::unordered_set<std::string> seen;
  for (const StoredAttachmentItem& item : items) {
    if (item.thread_id == thread_id && StoredKindOfMime(item.mime) == kind &&
        seen.insert(HashKey(item.content_hash)).second) {
      out.push_back(item);
    }
  }
  std::stable_sort(out.begin(), out.end(), NewerFirst);
  return out;
}

std::vector<StoredAttachmentItem> StorageItemsOfKind(const std::vector<StoredAttachmentItem>& items,
                                                     const StoredKind kind) {
  std::vector<StoredAttachmentItem> out;
  for (const StoredAttachmentItem& item : items) {
    if (StoredKindOfMime(item.mime) == kind) {
      out.push_back(item);
    }
  }
  return out;
}

std::vector<StoredAttachmentRef> StorageDeleteRefs(const std::vector<StoredAttachmentItem>& all,
                                                   const std::vector<StoredAttachmentItem>& selected,
                                                   const bool every_chat) {
  std::vector<StoredAttachmentRef> refs;
  std::set<std::pair<std::string, std::string>> added;
  const auto add = [&refs, &added](const StoredAttachmentItem& item) {
    if (added.emplace(item.thread_id, HashKey(item.content_hash)).second) {
      refs.emplace_back(item.thread_id, item.content_hash);
    }
  };
  if (!every_chat) {
    for (const StoredAttachmentItem& item : selected) {
      add(item);
    }
    return refs;
  }
  std::unordered_set<std::string> hashes;
  for (const StoredAttachmentItem& item : selected) {
    hashes.insert(HashKey(item.content_hash));
  }
  for (const StoredAttachmentItem& item : all) {
    if (hashes.count(HashKey(item.content_hash)) != 0) {
      add(item);
    }
  }
  return refs;
}

uint64_t StorageSelectedBytes(const std::vector<StoredAttachmentItem>& selected) {
  return StorageTotalsOf(selected).Bytes();
}

StorageDeleteFacts StorageDeleteFactsOf(const std::vector<StoredAttachmentItem>& all,
                                        const std::vector<StoredAttachmentItem>& selected,
                                        const std::vector<StoredAttachmentRef>& refs) {
  StorageDeleteFacts facts;
  std::set<std::pair<std::string, std::string>> deleting;
  for (const StoredAttachmentRef& ref : refs) {
    deleting.emplace(ref.first, HashKey(ref.second));
  }
  // Per hash: whether any deleted item has a server copy, and whether a chat not deleted now holds it.
  std::unordered_map<std::string, bool> server_copy;
  std::unordered_set<std::string> kept_elsewhere;
  for (const StoredAttachmentItem& item : all) {
    const std::string key = HashKey(item.content_hash);
    if (deleting.count({item.thread_id, key}) != 0) {
      server_copy[key] = server_copy[key] || item.has_server_copy;
    } else {
      kept_elsewhere.insert(key);
    }
  }
  std::unordered_set<std::string> seen;
  for (const StoredAttachmentItem& item : selected) {
    const std::string key = HashKey(item.content_hash);
    if (!seen.insert(key).second) {
      continue;
    }
    ++facts.count;
    const auto found = server_copy.find(key);
    const bool has_server_copy = found != server_copy.end() ? found->second : item.has_server_copy;
    if (!has_server_copy) {
      ++facts.only_here;
    }
    if (kept_elsewhere.count(key) != 0) {
      ++facts.shared_elsewhere;
    }
  }
  return facts;
}

StorageUsage ComputeStorageUsage(const uint64_t profile_bytes, const StorageKindTotals& attachments,
                                 const uint64_t message_store_bytes) {
  StorageUsage usage;
  usage.pictures = attachments.picture_bytes;
  usage.videos = attachments.video_bytes;
  usage.files = attachments.file_bytes;
  const uint64_t attachment_bytes = attachments.Bytes();
  usage.total = std::max(profile_bytes, attachment_bytes);
  const uint64_t rest = usage.total - attachment_bytes;
  usage.chats = std::min(message_store_bytes, rest);
  usage.other = rest - usage.chats;
  return usage;
}

std::vector<StorageBarSegment> StorageBarSegments(const StorageUsage& usage, const bool with_chats) {
  std::vector<StorageBarSegment> segments;
  const auto add = [&segments](const char* kind, const uint64_t bytes) {
    if (bytes > 0) {
      segments.push_back({.kind = kind, .bytes = bytes});
    }
  };
  add("picture", usage.pictures);
  add("video", usage.videos);
  add("file", usage.files);
  if (with_chats) {
    add("chats", usage.chats);
    add("other", usage.other);
  }
  uint64_t sum = 0;
  for (const StorageBarSegment& segment : segments) {
    sum += segment.bytes;
  }
  for (StorageBarSegment& segment : segments) {
    segment.percent = sum == 0 ? 0.f : static_cast<float>(static_cast<double>(segment.bytes) * 100.0 /
                                                          static_cast<double>(sum));
  }
  return segments;
}

StorageRecomputeGate::Decision StorageRecomputeGate::Next(const int64_t now_ms) {
  if (!stale_ || running_ || scheduled_) {
    return {};
  }
  const int64_t since = last_start_ms_ ? now_ms - *last_start_ms_ : interval_ms_;
  if (since >= interval_ms_) {
    return {.step = Step::RunNow};
  }
  scheduled_ = true;
  return {.step = Step::RunLater, .delay_ms = interval_ms_ - since};
}

StorageRecomputeGate::Decision StorageRecomputeGate::OnChange(const int64_t now_ms) {
  stale_ = true;
  return Next(now_ms);
}

void StorageRecomputeGate::OnStarted(const int64_t now_ms) {
  running_ = true;
  stale_ = false;
  last_start_ms_ = now_ms;
}

StorageRecomputeGate::Decision StorageRecomputeGate::OnFinished(const int64_t now_ms) {
  running_ = false;
  return Next(now_ms);
}

StorageRecomputeGate::Decision StorageRecomputeGate::OnTimer(const int64_t /*now_ms*/) {
  if (!scheduled_) {
    return {};
  }
  scheduled_ = false;
  // The timer was set for the end of the interval: a count running now picks the change up when it finishes.
  if (!stale_ || running_) {
    return {};
  }
  return {.step = Step::RunNow};
}

void StorageRecomputeGate::Reset() {
  running_ = false;
  scheduled_ = false;
  stale_ = false;
}

} // namespace pbr
