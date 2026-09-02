#include "domain/messaging/AttachmentSuppressionStore.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "domain/messaging/AttachmentCache.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

AttachmentSuppressionStore::AttachmentSuppressionStore(std::string profile_dir)
    : profile_dir_(std::move(profile_dir)) {}

void AttachmentSuppressionStore::SetProfileDir(std::string profile_dir) {
  std::lock_guard lock(mutex_);
  profile_dir_ = std::move(profile_dir);
  loaded_ = false;
  threads_.clear();
}

std::string AttachmentSuppressionStore::StorePath() const {
  return (std::filesystem::path(profile_dir_) / "attachment_suppressions.json").string();
}

std::string AttachmentSuppressionStore::HashKey(const std::vector<uint8_t>& content_hash) {
  return AttachmentHashHex(content_hash);
}

Roe<void> AttachmentSuppressionStore::LoadUnlocked() const {
  if (loaded_ || profile_dir_.empty()) {
    loaded_ = true;
    return Roe<void>{};
  }
  threads_.clear();
  const std::filesystem::path path(StorePath());
  if (!std::filesystem::exists(path)) {
    loaded_ = true;
    return Roe<void>{};
  }
  std::ifstream input(path);
  if (!input) {
    return Error("Failed to read attachment suppression store");
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  auto root = TryParseObject(buffer.str());
  if (!root) {
    return Error("Invalid attachment suppression store");
  }
  if (const Object* threads = root->getObject("threads")) {
    for (const auto& [thread_id, hashes_value] : threads->fields()) {
      const Array* hashes = asArray(hashes_value);
      if (!hashes) {
        continue;
      }
      auto& bucket = threads_[thread_id];
      for (const Value& item : hashes->elements) {
        if (auto hash = asString(item)) {
          bucket.insert(*hash);
        }
      }
    }
  }
  loaded_ = true;
  return Roe<void>{};
}

Roe<void> AttachmentSuppressionStore::SaveUnlocked() const {
  if (profile_dir_.empty()) {
    return Error("Attachment suppression profile directory is required");
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_dir_, ec);

  Object threads;
  for (const auto& [thread_id, hashes] : threads_) {
    std::vector<Value> array;
    array.reserve(hashes.size());
    for (const std::string& hash : hashes) {
      array.push_back(Value(hash));
    }
    threads.set(thread_id, ArrayValue(std::move(array)));
  }
  Object root;
  root.set("threads", ObjectValue(std::move(threads)));

  const auto path = std::filesystem::path(StorePath());
  const auto temp = path.string() + ".tmp";
  {
    std::ofstream output(temp, std::ios::trunc);
    if (!output) {
      return Error("Failed to write attachment suppression store");
    }
    output << DumpJson(root, 2);
    if (!output) {
      return Error("Failed to write attachment suppression store");
    }
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    return Error("Failed to write attachment suppression store");
  }
  return Roe<void>{};
}

bool AttachmentSuppressionStore::IsSuppressed(const std::string& thread_id,
                                              const std::vector<uint8_t>& content_hash) const {
  if (thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return false;
  }
  std::lock_guard lock(mutex_);
  if (auto loaded = LoadUnlocked(); !loaded) {
    return false;
  }
  const auto thread_it = threads_.find(thread_id);
  if (thread_it == threads_.end()) {
    return false;
  }
  return thread_it->second.contains(HashKey(content_hash));
}

Roe<void> AttachmentSuppressionStore::Suppress(const std::string& thread_id,
                                               const std::vector<uint8_t>& content_hash) {
  if (thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment suppression target");
  }
  std::lock_guard lock(mutex_);
  if (auto loaded = LoadUnlocked(); !loaded) {
    return loaded.error();
  }
  threads_[thread_id].insert(HashKey(content_hash));
  return SaveUnlocked();
}

Roe<void> AttachmentSuppressionStore::ClearSuppression(const std::string& thread_id,
                                                     const std::vector<uint8_t>& content_hash) {
  if (thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return Error("Invalid attachment suppression target");
  }
  std::lock_guard lock(mutex_);
  if (auto loaded = LoadUnlocked(); !loaded) {
    return loaded.error();
  }
  const auto thread_it = threads_.find(thread_id);
  if (thread_it != threads_.end()) {
    thread_it->second.erase(HashKey(content_hash));
    if (thread_it->second.empty()) {
      threads_.erase(thread_it);
    }
  }
  return SaveUnlocked();
}

Roe<void> AttachmentSuppressionStore::SuppressAll(const std::string& thread_id,
                                                  const std::vector<std::vector<uint8_t>>& content_hashes) {
  if (thread_id.empty()) {
    return Error("thread_id is required");
  }
  std::lock_guard lock(mutex_);
  if (auto loaded = LoadUnlocked(); !loaded) {
    return loaded.error();
  }
  auto& bucket = threads_[thread_id];
  for (const auto& hash : content_hashes) {
    if (hash.size() == kAttachmentContentHashSize) {
      bucket.insert(HashKey(hash));
    }
  }
  return SaveUnlocked();
}

Roe<void> AttachmentSuppressionStore::ClearThread(const std::string& thread_id) {
  if (thread_id.empty()) {
    return Error("thread_id is required");
  }
  std::lock_guard lock(mutex_);
  if (auto loaded = LoadUnlocked(); !loaded) {
    return loaded.error();
  }
  threads_.erase(thread_id);
  return SaveUnlocked();
}

} // namespace pbr
