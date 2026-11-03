#pragma once

#include "domain/attachments/StoredAttachments.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** A chat's name as the chat list shows it, and the seed of its letter avatar. */
struct StorageChatName {
  std::string thread_id;
  std::string name;
  std::string avatar_seed;
};

/**
 * Me → Storage → Manage storage ports. Application fills them from the messaging facade; every async `done`
 * runs on the UI thread. Unbound ports leave the pages empty.
 */
struct StorageManagerPorts {
  /** The attachments this device holds, one per (chat, hash) (ListStoredAttachmentsAsync). */
  std::function<void(std::function<void(std::vector<StoredAttachmentItem>)> done)> list_stored;
  /** What deleting `items` would really free. */
  std::function<void(std::vector<StoredAttachmentRef> items, std::function<void(uint64_t)> done)> releasable_bytes;
  std::function<void(std::vector<StoredAttachmentRef> items, std::function<void(StoredAttachmentDeleteResult)> done)>
      delete_local;
  /** Bytes of the message databases. */
  std::function<void(std::function<void(uint64_t)> done)> message_store_bytes;
  /** Bytes of the whole profile folder. */
  std::function<void(std::function<void(uint64_t)> done)> profile_bytes;
  /** UI thread: the chats' display names. */
  std::function<std::vector<StorageChatName>()> chat_names;
  std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash, const std::string& mime,
                     uint64_t page_generation)>
      request_thumbnail;
  std::function<void(uint64_t page_generation)> cancel_thumbnails;
  /** UI thread, a stat only: the cell's thumbnail or video poster ("" when none yet). */
  std::function<std::string(const std::string& thread_id, const std::vector<uint8_t>& content_hash)> thumbnail_path;
  /**
   * The file to hand to the system, made off the UI thread like a tap in the chat (decrypting when needed);
   * `done`: its path ("" when it cannot be made) and whether to ask before opening it in another app.
   */
  std::function<void(const StoredAttachmentItem& item, std::function<void(std::string path, bool needs_confirm)> done)>
      open_item;
  /** Calls `on_changed` on the UI thread whenever an attachment arrives, goes, or gets a thumbnail. */
  std::function<void(std::function<void()> on_changed)> set_on_changed;
  /** Runs `task` on the UI thread after `delay`. */
  std::function<void(std::chrono::milliseconds delay, std::function<void()> task)> run_later;
};

} // namespace pbr
