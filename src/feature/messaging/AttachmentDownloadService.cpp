#include "feature/messaging/AttachmentDownloadService.h"

#include "base/crypto/AttachmentContentHash.h"
#include "base/messaging/AttachmentCache.h"
#include "base/messaging/ChatPayloadCodec.h"
#include "base/net/AttachmentFetchUtil.h"
#include "base/runtime/AppRuntime.h"

namespace pbr {

void AttachmentDownloadService::SetProfileDataDir(std::string profile_dir) {
  profile_dir_ = std::move(profile_dir);
}

void AttachmentDownloadService::SetOnChanged(ChangedCallback callback) {
  on_changed_ = std::move(callback);
}

std::string AttachmentDownloadService::JobKey(const Job& job) const {
  return job.thread_id + "|" + AttachmentHashHex(job.fields.content_hash);
}

void AttachmentDownloadService::EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message) {
  if (message.content_type != ChatContentType::Attachment) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields) {
    return;
  }
  EnqueueJob(Job{.thread_id = thread_id, .message_id = message.id, .fields = std::move(*fields)});
}

void AttachmentDownloadService::EnqueueJob(Job job) {
  if (profile_dir_.empty() || job.thread_id.empty() || job.fields.url.empty()) {
    return;
  }
  if (!AttachmentLocalPath(profile_dir_, job.thread_id, job.fields.content_hash, job.fields.mime,
                           job.fields.filename)
           .empty()) {
    return;
  }

  const std::string key = JobKey(job);
  bool should_start = false;
  {
    std::lock_guard lock(mutex_);
    if (active_.contains(key)) {
      return;
    }
    for (const Job& queued : queue_) {
      if (JobKey(queued) == key) {
        return;
      }
    }
    failed_.erase(key);
    queue_.push_back(std::move(job));
    should_start = !draining_;
    draining_ = true;
  }

  if (should_start) {
    AppRuntime::PostWorkerNormal([this]() { DrainQueue(); });
  }
}

void AttachmentDownloadService::DrainQueue() {
  for (;;) {
    Job job;
    {
      std::lock_guard lock(mutex_);
      if (queue_.empty()) {
        draining_ = false;
        return;
      }
      job = std::move(queue_.front());
      queue_.erase(queue_.begin());
      active_.insert(JobKey(job));
    }
    RunJob(job);
  }
}

void AttachmentDownloadService::RunJob(const Job& job) {
  const std::string key = JobKey(job);
  auto plaintext = FetchAndDecryptAttachment(job.fields);
  if (!plaintext) {
    MarkFailed(key);
    return;
  }
  const ByteVector bytes(plaintext->begin(), plaintext->end());
  if (auto saved = SaveAttachmentPlaintext(profile_dir_, job.thread_id, job.fields.content_hash, job.fields.mime,
                                           bytes, job.fields.filename);
      !saved) {
    MarkFailed(key);
    return;
  }
  MarkReady(key);
}

void AttachmentDownloadService::MarkFailed(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    active_.erase(key);
    failed_.insert(key);
  }
  NotifyChanged();
}

void AttachmentDownloadService::MarkReady(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    active_.erase(key);
    failed_.erase(key);
  }
  NotifyChanged();
}

void AttachmentDownloadService::NotifyChanged() {
  if (!on_changed_) {
    return;
  }
  AppRuntime::PostUI([cb = on_changed_]() {
    if (cb) {
      cb();
    }
  });
}

AttachmentDownloadService::DownloadState AttachmentDownloadService::StateFor(
    const std::string& thread_id, const std::vector<uint8_t>& content_hash) const {
  if (profile_dir_.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return DownloadState::Failed;
  }
  if (!AttachmentLocalPath(profile_dir_, thread_id, content_hash, "", {}).empty()) {
    return DownloadState::Ready;
  }
  const std::string key = thread_id + "|" + AttachmentHashHex(content_hash);
  std::lock_guard lock(mutex_);
  if (failed_.contains(key)) {
    return DownloadState::Failed;
  }
  return DownloadState::Downloading;
}

void AttachmentDownloadService::RetryDownload(const std::string& thread_id, const std::string& message_id,
                                              IThreadStore& store) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return;
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id) {
      continue;
    }
    EnqueueFromMessage(thread_id, message);
    return;
  }
}

void AttachmentDownloadService::EnsureThreadQueued(const std::string& thread_id, IThreadStore& store) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return;
  }
  for (const ThreadMessage& message : *page) {
    EnqueueFromMessage(thread_id, message);
  }
}

} // namespace pbr
