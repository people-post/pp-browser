#pragma once

#include "common/Error.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Profile-local tombstones for attachment hashes (R020). */
class AttachmentSuppressionStore {
public:
  explicit AttachmentSuppressionStore(std::string profile_dir);

  void SetProfileDir(std::string profile_dir);

  bool IsSuppressed(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  Roe<void> Suppress(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  Roe<void> ClearSuppression(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  Roe<void> SuppressAll(const std::string& thread_id, const std::vector<std::vector<uint8_t>>& content_hashes);
  Roe<void> ClearThread(const std::string& thread_id);

private:
  Roe<void> LoadUnlocked() const;
  Roe<void> SaveUnlocked() const;
  std::string StorePath() const;
  static std::string HashKey(const std::vector<uint8_t>& content_hash);

  mutable std::mutex mutex_;
  std::string profile_dir_;
  mutable bool loaded_ = false;
  mutable std::unordered_map<std::string, std::unordered_set<std::string>> threads_;
};

} // namespace pbr
