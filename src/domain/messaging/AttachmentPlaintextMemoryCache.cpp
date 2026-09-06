#include "domain/messaging/AttachmentPlaintextMemoryCache.h"

namespace pbr {

AttachmentPlaintextMemoryCache& AttachmentPlaintextMemoryCache::Instance() {
  static AttachmentPlaintextMemoryCache cache;
  return cache;
}

void AttachmentPlaintextMemoryCache::TouchLocked(std::list<std::string>::iterator it) {
  order_.splice(order_.begin(), order_, it);
}

void AttachmentPlaintextMemoryCache::EvictWhileNeededLocked() {
  while (!order_.empty() && (map_.size() > kMaxEntries || total_bytes_ > kMaxTotalBytes)) {
    const std::string& victim = order_.back();
    auto found = map_.find(victim);
    if (found != map_.end()) {
      total_bytes_ -= found->second.bytes.size();
      map_.erase(found);
    }
    order_.pop_back();
  }
}

void AttachmentPlaintextMemoryCache::Put(const std::string& content_hash_hex, const ByteVector& bytes) {
  if (content_hash_hex.empty() || bytes.empty() || bytes.size() > kMaxEntryBytes) {
    return;
  }
  std::lock_guard lock(mutex_);
  auto existing = map_.find(content_hash_hex);
  if (existing != map_.end()) {
    total_bytes_ -= existing->second.bytes.size();
    existing->second.bytes = bytes;
    total_bytes_ += bytes.size();
    TouchLocked(existing->second.order_it);
    EvictWhileNeededLocked();
    return;
  }
  order_.push_front(content_hash_hex);
  Entry entry;
  entry.bytes = bytes;
  entry.order_it = order_.begin();
  map_.emplace(content_hash_hex, std::move(entry));
  total_bytes_ += bytes.size();
  EvictWhileNeededLocked();
}

bool AttachmentPlaintextMemoryCache::TryGet(const std::string& content_hash_hex, ByteVector& out) {
  if (content_hash_hex.empty()) {
    return false;
  }
  std::lock_guard lock(mutex_);
  auto found = map_.find(content_hash_hex);
  if (found == map_.end()) {
    return false;
  }
  TouchLocked(found->second.order_it);
  out = found->second.bytes;
  return true;
}

void AttachmentPlaintextMemoryCache::Clear() {
  std::lock_guard lock(mutex_);
  map_.clear();
  order_.clear();
  total_bytes_ = 0;
}

std::size_t AttachmentPlaintextMemoryCache::EntryCountForTest() const {
  std::lock_guard lock(mutex_);
  return map_.size();
}

std::size_t AttachmentPlaintextMemoryCache::TotalBytesForTest() const {
  std::lock_guard lock(mutex_);
  return total_bytes_;
}

} // namespace pbr
