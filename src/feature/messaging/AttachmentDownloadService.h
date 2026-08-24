#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/IThreadStore.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

/** Background CDN fetch + decrypt for chat attachments (R008). */
class AttachmentDownloadService {
public:
  enum class DownloadState { Ready, Downloading, Failed };

  using ChangedCallback = std::function<void()>;

  void SetProfileDataDir(std::string profile_dir);
  void SetOnChanged(ChangedCallback callback);

  void EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message);
  void RetryDownload(const std::string& thread_id, const std::string& message_id, IThreadStore& store);

  /** Queue any attachment messages in the thread that are not cached yet. */
  void EnsureThreadQueued(const std::string& thread_id, IThreadStore& store);

  DownloadState StateFor(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;

private:
  struct Job {
    std::string thread_id;
    std::string message_id;
    ChatAttachmentFields fields;
  };

  std::string JobKey(const Job& job) const;
  void EnqueueJob(Job job);
  void DrainQueue();
  void RunJob(const Job& job);
  void MarkFailed(const std::string& key);
  void MarkReady(const std::string& key);
  void NotifyChanged();

  std::string profile_dir_;
  ChangedCallback on_changed_;
  mutable std::mutex mutex_;
  std::vector<Job> queue_;
  std::unordered_set<std::string> active_;
  std::unordered_set<std::string> failed_;
  bool draining_ = false;
};

} // namespace pbr
