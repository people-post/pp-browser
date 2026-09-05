#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

namespace pbr {

/**
 * Process-wide LRU of private attachment plaintext (images / small videos / posters).
 * Keyed by content-hash hex. Cleared on ClearDek via WipeAllAttachmentViewCaches.
 * Large objects are not admitted (presentation still uses CAS + optional blobs_view).
 */
class AttachmentPlaintextMemoryCache {
public:
  static AttachmentPlaintextMemoryCache& Instance();

  /** Cap entries so chat scroll does not unbounded-grow RAM. */
  static constexpr std::size_t kMaxEntries = 64;
  /** Soft total budget across admitted entries. */
  static constexpr std::size_t kMaxTotalBytes = 64ull * 1024ull * 1024ull;
  /** Skip caching a single object above this (large videos stay on CAS only). */
  static constexpr std::size_t kMaxEntryBytes = 8ull * 1024ull * 1024ull;

  void Put(const std::string& content_hash_hex, const ByteVector& bytes);
  bool TryGet(const std::string& content_hash_hex, ByteVector& out);
  void Clear();

  std::size_t EntryCountForTest() const;
  std::size_t TotalBytesForTest() const;

private:
  AttachmentPlaintextMemoryCache() = default;

  void TouchLocked(std::list<std::string>::iterator it);
  void EvictWhileNeededLocked();

  mutable std::mutex mutex_;
  std::list<std::string> order_; // front = most recently used
  struct Entry {
    ByteVector bytes;
    std::list<std::string>::iterator order_it;
  };
  std::unordered_map<std::string, Entry> map_;
  std::size_t total_bytes_ = 0;
};

} // namespace pbr
