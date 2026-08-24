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

void AttachmentDownloadService::SetFetchDependencies(IThreadStore* store, ContactsStore* contacts,
                                                     IdentityStore* identity, IChatBlobPeerClient* peer_client) {
  store_ = store;
  contacts_ = contacts;
  identity_ = identity;
  peer_client_ = peer_client;
}

void AttachmentDownloadService::SetSuppressionStore(AttachmentSuppressionStore* suppression) {
  suppression_ = suppression;
}

void AttachmentDownloadService::SetDownloadPolicy(const AttachmentDownloadPolicy policy) {
  policy_ = policy;
}

void AttachmentDownloadService::SetBacklogDrainActive(const bool active) {
  backlog_drain_ = active;
}

void AttachmentDownloadService::SetOnChanged(ChangedCallback callback) {
  on_changed_ = std::move(callback);
}

std::string AttachmentDownloadService::JobKey(const Job& job) const {
  return job.thread_id + "|" + AttachmentHashHex(job.fields.content_hash);
}

std::string AttachmentDownloadService::JobKey(const std::string& thread_id,
                                             const std::vector<uint8_t>& content_hash) const {
  return thread_id + "|" + AttachmentHashHex(content_hash);
}

bool AttachmentDownloadService::ShouldAutoDownload(const Job& job) const {
  if (suppression_ && suppression_->IsSuppressed(job.thread_id, job.fields.content_hash)) {
    return false;
  }
  return ShouldAutoEnqueueAttachment(policy_, job.fields.byte_length, backlog_drain_);
}

void AttachmentDownloadService::EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message,
                                                   const bool force) {
  if (message.content_type != ChatContentType::Attachment) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields) {
    return;
  }
  EnqueueJob(Job{.thread_id = thread_id, .message_id = message.id, .fields = std::move(*fields)}, force);
}

void AttachmentDownloadService::EnqueueJob(Job job, const bool force) {
  if (profile_dir_.empty() || job.thread_id.empty()) {
    return;
  }
  AttachmentFetchContext fetch_context;
  fetch_context.thread_id = job.thread_id;
  fetch_context.store = store_;
  fetch_context.peer_client = peer_client_;
  fetch_context.profile_data_dir = profile_dir_;
  if (!CanFetchAttachment(job.fields, fetch_context)) {
    return;
  }
  if (!AttachmentLocalPath(profile_dir_, job.thread_id, job.fields.content_hash, job.fields.mime,
                           job.fields.filename)
           .empty()) {
    return;
  }

  const std::string key = JobKey(job);
  if (!force && suppression_ && suppression_->IsSuppressed(job.thread_id, job.fields.content_hash)) {
    MarkPending(key);
    return;
  }
  if (!force && !ShouldAutoDownload(job)) {
    MarkPending(key);
    return;
  }

  bool should_start = false;
  {
    std::lock_guard lock(mutex_);
    pending_.erase(key);
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
  AttachmentFetchContext context;
  context.thread_id = job.thread_id;
  context.store = store_;
  context.contacts = contacts_;
  context.identity = identity_;
  context.peer_client = peer_client_;
  context.profile_data_dir = profile_dir_;
  auto plaintext = FetchAndDecryptAttachment(job.fields, context);
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
    pending_.erase(key);
    failed_.insert(key);
  }
  NotifyChanged();
}

void AttachmentDownloadService::MarkReady(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    active_.erase(key);
    pending_.erase(key);
    failed_.erase(key);
  }
  NotifyChanged();
}

void AttachmentDownloadService::MarkPending(const std::string& key) {
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
    if (failed_.contains(key)) {
      return;
    }
    if (!pending_.insert(key).second) {
      return;
    }
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
    const std::string& thread_id, const std::vector<uint8_t>& content_hash, const uint64_t byte_length) const {
  if (profile_dir_.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return DownloadState::Failed;
  }
  if (!AttachmentLocalPath(profile_dir_, thread_id, content_hash, "", {}).empty()) {
    return DownloadState::Ready;
  }
  const std::string key = JobKey(thread_id, content_hash);
  std::lock_guard lock(mutex_);
  if (failed_.contains(key)) {
    return DownloadState::Failed;
  }
  if (active_.contains(key)) {
    return DownloadState::Downloading;
  }
  for (const Job& queued : queue_) {
    if (JobKey(queued) == key) {
      return DownloadState::Downloading;
    }
  }
  if (pending_.contains(key)) {
    return DownloadState::Pending;
  }
  if (suppression_ && suppression_->IsSuppressed(thread_id, content_hash)) {
    return DownloadState::Pending;
  }
  if (!ShouldAutoEnqueueAttachment(policy_, byte_length, backlog_drain_)) {
    return DownloadState::Pending;
  }
  return DownloadState::Downloading;
}

void AttachmentDownloadService::RetryDownload(const std::string& thread_id, const std::string& message_id,
                                              IThreadStore& store) {
  RequestDownload(thread_id, message_id, store);
}

void AttachmentDownloadService::RequestDownload(const std::string& thread_id, const std::string& message_id,
                                                IThreadStore& store) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return;
  }
  for (const ThreadMessage& message : *page) {
    if (message.id != message_id || message.content_type != ChatContentType::Attachment) {
      continue;
    }
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
    if (!fields) {
      return;
    }
    if (suppression_) {
      (void)suppression_->ClearSuppression(thread_id, fields->content_hash);
    }
    EnqueueFromMessage(thread_id, message, /*force=*/true);
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

void AttachmentDownloadService::DrainPendingMediaBacklog(IThreadStore& store) {
  const bool previous = backlog_drain_;
  backlog_drain_ = true;
  if (auto threads = store.ListThreads()) {
    for (const Thread& thread : *threads) {
      EnsureThreadQueued(thread.id, store);
    }
  }
  backlog_drain_ = previous;
}

Roe<void> AttachmentDownloadService::PrepareThreadHistoryClear(const std::string& thread_id, IThreadStore& store) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return page.error();
  }

  std::vector<std::vector<uint8_t>> hashes;
  hashes.reserve(page->size());
  for (const ThreadMessage& message : *page) {
    if (message.content_type != ChatContentType::Attachment) {
      continue;
    }
    auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
    if (!fields || fields->content_hash.size() != kAttachmentContentHashSize) {
      continue;
    }
    hashes.push_back(fields->content_hash);
  }

  if (suppression_ && !hashes.empty()) {
    if (auto suppressed = suppression_->SuppressAll(thread_id, hashes); !suppressed) {
      return suppressed.error();
    }
  }

  {
    std::lock_guard lock(mutex_);
    for (auto it = queue_.begin(); it != queue_.end();) {
      if (it->thread_id == thread_id) {
        it = queue_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = active_.begin(); it != active_.end();) {
      if (it->find(thread_id + "|") == 0) {
        it = active_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = failed_.begin(); it != failed_.end();) {
      if (it->find(thread_id + "|") == 0) {
        it = failed_.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = pending_.begin(); it != pending_.end();) {
      if (it->find(thread_id + "|") == 0) {
        it = pending_.erase(it);
      } else {
        ++it;
      }
    }
  }

  return WipeThreadAttachmentBlobs(profile_dir_, thread_id);
}

} // namespace pbr
