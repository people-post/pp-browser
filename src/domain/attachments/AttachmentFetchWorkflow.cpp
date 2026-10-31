#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/AttachmentContent.h"

#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlaintextMemoryCache.h"
#include "lib/content/CasStore.h"
#include "lib/messaging/AiImageRef.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "domain/attachments/AttachmentFetch.h"
#include "foundation/runtime/AppRuntime.h"

#include <sodium.h>

#include <chrono>
#include <filesystem>
#include <unordered_set>

namespace pbr {
namespace {

/** Content hashes a message refers to (attachment, AI image). */
std::vector<std::vector<uint8_t>> MessageContentHashes(const ThreadMessage& message);
/** Bytes a cancelled download wrote: CAS object, view, poster, RAM copy. */
void DeleteFetchedBytes(const std::string& profile_dir, const std::string& profile_id, const std::string& thread_id,
                        const ChatAttachmentFields& fields);

} // namespace
} // namespace pbr
#include "common/PbrCompat.h"

namespace pbr {

void AttachmentFetchWorkflow::SetProfileDataDir(std::string profile_dir) {
  profile_dir_ = std::move(profile_dir);
}

void AttachmentFetchWorkflow::SetProfileId(std::string profile_id) {
  profile_id_ = std::move(profile_id);
}

void AttachmentFetchWorkflow::SetFetchDependencies(IThreadStore* store, ContactsStore* contacts,
                                                     IdentityStore* identity, IChatBlobPeerClient* peer_client) {
  store_ = store;
  contacts_ = contacts;
  identity_ = identity;
  peer_client_ = peer_client;
}

void AttachmentFetchWorkflow::SetSuppressionStore(AttachmentSuppressionStore* suppression) {
  suppression_ = suppression;
}

void AttachmentFetchWorkflow::SetDownloadPolicy(const AttachmentDownloadPolicy policy) {
  policy_ = policy;
}

void AttachmentFetchWorkflow::SetBacklogDrainActive(const bool active) {
  backlog_drain_ = active;
}

void AttachmentFetchWorkflow::SetLimits(const AttachmentLimits& limits) {
  std::lock_guard lock(mutex_);
  limits_ = limits;
}

AttachmentLimits AttachmentFetchWorkflow::Limits() const {
  std::lock_guard lock(mutex_);
  return limits_;
}

void AttachmentFetchWorkflow::SetDiskUsageProvider(std::function<uint64_t()> provider) {
  std::lock_guard lock(mutex_);
  disk_usage_ = std::move(provider);
}

void AttachmentFetchWorkflow::SetStoreScanForTest(std::function<uint64_t()> scan) {
  std::lock_guard lock(mutex_);
  scan_override_ = std::move(scan);
}

void AttachmentFetchWorkflow::SetClockForTest(std::function<int64_t()> now_ms) {
  std::lock_guard lock(mutex_);
  clock_ = std::move(now_ms);
}

namespace {

int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** What the attachment store holds: the private content blocks plus pushed ciphertext not yet opened. */
uint64_t ScanAttachmentStoreBytes(const std::string& profile_dir, const std::string& profile_id) {
  if (profile_dir.empty()) {
    return 0;
  }
  uint64_t total = PendingAttachmentCiphertextBytes(profile_dir);
  if (!profile_id.empty()) {
    std::error_code ec;
    CasStore cas(profile_dir, profile_id);
    for (std::filesystem::recursive_directory_iterator it(cas.BlocksRoot(CasRealm::Private), ec), end;
         !ec && it != end; it.increment(ec)) {
      std::error_code size_ec;
      if (it->is_regular_file(size_ec) && !size_ec) {
        total += it->file_size(size_ec);
      }
    }
  }
  return total;
}

} // namespace

bool AttachmentFetchWorkflow::AdmitByBudget(AutoDownloadBudget& budget, const std::string& sender,
                                            const uint64_t byte_length) {
  AttachmentLimits limits;
  std::function<uint64_t()> provider;
  std::function<int64_t()> clock;
  std::function<uint64_t()> scan_override;
  bool need_scan = false;
  std::string profile_dir;
  std::string profile_id;
  {
    std::lock_guard lock(mutex_);
    limits = limits_;
    provider = disk_usage_;
    clock = clock_;
    scan_override = scan_override_;
    profile_dir = profile_dir_;
    profile_id = profile_id_;
  }
  const int64_t now_ms = clock ? clock() : SteadyNowMs();
  uint64_t used = 0;
  if (provider) {
    used = provider();
  } else {
    {
      std::lock_guard lock(mutex_);
      need_scan = scanned_at_ms_ == 0 || now_ms - scanned_at_ms_ >= 30 * 1000;
    }
    if (need_scan) {
      // A directory walk: never under mutex_, which StateFor (the transcript render) takes.
      const uint64_t total = scan_override ? scan_override() : ScanAttachmentStoreBytes(profile_dir, profile_id);
      std::lock_guard lock(mutex_);
      scanned_disk_bytes_ = total;
      scanned_at_ms_ = now_ms;
      started_since_scan_bytes_ = 0;
    }
    std::lock_guard lock(mutex_);
    used = scanned_disk_bytes_ + started_since_scan_bytes_;
  }
  if (used >= limits.auto_download_disk_cap_bytes) {
    return false;
  }
  if (!budget.TryAcquire(sender, byte_length, now_ms, limits.auto_download_max_count_per_minute,
                         limits.auto_download_max_bytes_per_minute)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  started_since_scan_bytes_ += byte_length;
  return true;
}

bool AttachmentFetchWorkflow::TryAutoDownloadBudget(const std::string& sender, const uint64_t byte_length) {
  return AdmitByBudget(budget_, sender, byte_length);
}

bool AttachmentFetchWorkflow::TryAcceptDirectPush(const std::string& sender, const uint64_t ciphertext_bytes) {
  return AdmitByBudget(push_budget_, sender, ciphertext_bytes);
}

void AttachmentFetchWorkflow::SetOnChanged(ChangedCallback callback) {
  on_changed_ = std::move(callback);
}

Roe<void> AttachmentFetchWorkflow::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  {
    std::lock_guard lock(mutex_);
    if (!dek_.empty()) {
      sodium_memzero(dek_.data(), dek_.size());
    }
    dek_ = std::move(dek);
  }
  return {};
}

void AttachmentFetchWorkflow::ClearDek() {
  {
    std::lock_guard lock(mutex_);
    if (!dek_.empty()) {
      sodium_memzero(dek_.data(), dek_.size());
      dek_.clear();
    }
  }
  if (!profile_dir_.empty()) {
    (void)WipeAllAttachmentViewCaches(profile_dir_);
  }
}

bool AttachmentFetchWorkflow::HasDek() const {
  std::lock_guard lock(mutex_);
  return dek_.size() == kDataEncryptionKeySize;
}

ByteVector AttachmentFetchWorkflow::CopyDekUnlocked() const {
  if (dek_.size() != kDataEncryptionKeySize) {
    return {};
  }
  return dek_;
}

ByteVector AttachmentFetchWorkflow::CopyDek() const {
  std::lock_guard lock(mutex_);
  return CopyDekUnlocked();
}

std::string AttachmentFetchWorkflow::JobKey(const Job& job) const {
  return job.thread_id + "|" + AttachmentHashHex(job.fields.content_hash);
}

std::string AttachmentFetchWorkflow::JobKey(const std::string& thread_id,
                                             const std::vector<uint8_t>& content_hash) const {
  return thread_id + "|" + AttachmentHashHex(content_hash);
}

bool AttachmentFetchWorkflow::ShouldAutoDownload(const Job& job) const {
  if (job.fields.byte_length == 0) {
    return false; // an undeclared size is the sender's word for nothing: only a tap fetches it
  }
  if (suppression_ && suppression_->IsSuppressed(job.thread_id, job.fields.content_hash)) {
    return false;
  }
  return ShouldAutoEnqueueAttachment(policy_, job.fields.byte_length, backlog_drain_, Limits().auto_download_max_bytes);
}

void AttachmentFetchWorkflow::EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message,
                                                   const bool force) {
  if (message.content_type != ChatContentType::Attachment) {
    return;
  }
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
  if (!fields) {
    return;
  }
  EnqueueJob(Job{.thread_id = thread_id,
                 .message_id = message.id,
                 .sender = message.sender_contact_id,
                 .fields = std::move(*fields)},
             force);
}

void AttachmentFetchWorkflow::EnqueueJob(Job job, const bool force) {
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
  if (AttachmentBlobExists(profile_dir_, job.fields.content_hash)) {
    return;
  }
  if (job.fields.byte_length > Limits().max_receive_bytes) {
    return; // never downloaded, even on request: the card says it is too large
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
  if (!force) {
    bool already_throttled = false;
    {
      std::lock_guard lock(mutex_);
      // Already downloading or queued: a repeat look must not spend the sender's budget again.
      if (active_.contains(key)) {
        return;
      }
      for (const Job& queued : queue_) {
        if (JobKey(queued) == key) {
          return;
        }
      }
      already_throttled = throttled_.contains(key);
    }
    // A sender over the automatic limits (or a full store) leaves the file as a "Download" row.
    if (already_throttled || !TryAutoDownloadBudget(job.sender, job.fields.byte_length)) {
      {
        std::lock_guard lock(mutex_);
        throttled_.insert(key);
      }
      MarkPending(key);
      return;
    }
  }

  bool should_start = false;
  {
    std::lock_guard lock(mutex_);
    pending_.erase(key);
    throttled_.erase(key); // asked for (or allowed): no longer held back
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

void AttachmentFetchWorkflow::DrainQueue() {
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
  RunJobAsync(std::move(job), [this]() { ContinueDrainOnWorker(); });
}

void AttachmentFetchWorkflow::ContinueDrainOnWorker() {
  if (AppRuntime::IsRunning()) {
    AppRuntime::PostWorkerNormal([this]() { DrainQueue(); });
  } else {
    DrainQueue();
  }
}

void AttachmentFetchWorkflow::RunJobAsync(Job job, std::function<void()> on_done) {
  const std::string key = JobKey(job);
  AttachmentFetchContext context;
  context.thread_id = job.thread_id;
  context.store = store_;
  context.contacts = contacts_;
  context.identity = identity_;
  context.peer_client = peer_client_;
  context.profile_data_dir = profile_dir_;

  FetchAndDecryptAttachmentAsync(
      job.fields, context,
      [this, key, job = std::move(job), on_done = std::move(on_done)](Roe<std::vector<uint8_t>> plaintext) mutable {
        auto finish_job = [this, key, job = std::move(job), on_done = std::move(on_done),
                           plaintext = std::move(plaintext)]() mutable {
          if (!plaintext) {
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          bool cancelled = false;
          {
            std::lock_guard lock(mutex_);
            cancelled = !active_.contains(key); // message / thread deleted or history cleared meanwhile
          }
          if (cancelled) {
            if (on_done) {
              on_done();
            }
            return;
          }
          const ByteVector bytes(plaintext->begin(), plaintext->end());
          const ByteVector dek_copy = CopyDek();
          if (dek_copy.empty() || profile_id_.empty()) {
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          if (auto saved = SaveAttachmentPlaintext(profile_dir_, profile_id_, job.fields.content_hash, job.fields.mime, bytes, job.fields.filename, dek_copy);
              !saved) {
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          const uint64_t size_hint = job.fields.byte_length > 0 ? job.fields.byte_length : bytes.size();
          if (AttachmentAllowsInlinePrivateView(job.fields.mime, size_hint, Limits().max_receive_bytes)) {
            (void)EnsureAttachmentView(profile_dir_, profile_id_, job.thread_id, job.fields.content_hash, job.fields.mime, job.fields.filename, dek_copy);
          }
          MaybeBuildPoster(job.thread_id, job.fields);
          // Cancelled while writing (message deleted): take back what was just written.
          bool cancelled_meanwhile = false;
          {
            std::lock_guard lock(mutex_);
            cancelled_meanwhile = !active_.contains(key);
          }
          if (cancelled_meanwhile) {
            DeleteFetchedBytes(profile_dir_, profile_id_, job.thread_id, job.fields);
            if (on_done) {
              on_done();
            }
            return;
          }
          MarkReady(key);
          if (on_done) {
            on_done();
          }
        };
        // Amp blob completions may arrive off-worker; keep SQLite/DEK work on Normal.
        if (AppRuntime::IsRunning()) {
          AppRuntime::PostWorkerNormal(std::move(finish_job));
        } else {
          finish_job();
        }
      });
}

void AttachmentFetchWorkflow::MarkFailed(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    active_.erase(key);
    pending_.erase(key);
    failed_.insert(key);
  }
  NotifyChanged();
}

void AttachmentFetchWorkflow::MarkReady(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    active_.erase(key);
    pending_.erase(key);
    failed_.erase(key);
  }
  NotifyChanged();
}

void AttachmentFetchWorkflow::MarkPending(const std::string& key) {
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

void AttachmentFetchWorkflow::NotifyChanged() {
  if (!on_changed_) {
    return;
  }
  AppRuntime::PostUI([cb = on_changed_]() {
    if (cb) {
      cb();
    }
  });
}

AttachmentFetchWorkflow::DownloadState AttachmentFetchWorkflow::StateFor(
    const std::string& thread_id, const std::vector<uint8_t>& content_hash, const uint64_t byte_length) const {
  if (profile_dir_.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return DownloadState::Failed;
  }
  if (AttachmentBlobExists(profile_dir_, content_hash)) {
    return DownloadState::Ready;
  }
  const std::string key = JobKey(thread_id, content_hash);
  std::lock_guard lock(mutex_);
  if (byte_length > limits_.max_receive_bytes) {
    return DownloadState::TooLarge;
  }
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
  if (throttled_.contains(key)) {
    return DownloadState::Pending;
  }
  if (byte_length == 0 ||
      !ShouldAutoEnqueueAttachment(policy_, byte_length, backlog_drain_, limits_.auto_download_max_bytes)) {
    return DownloadState::Pending;
  }
  return DownloadState::Downloading;
}

Roe<std::string> AttachmentFetchWorkflow::EnsureLocalViewPath(const std::string& thread_id,
                                                                const std::vector<uint8_t>& content_hash,
                                                                const std::string& mime,
                                                                const std::string& filename) {
  const ByteVector dek_copy = CopyDek();
  if (dek_copy.empty() || profile_id_.empty()) {
    return Error("Attachment view requires unlocked DEK");
  }
  return EnsureAttachmentView(profile_dir_, profile_id_, thread_id, content_hash, mime, filename, dek_copy);
}

Roe<AttachmentFetchWorkflow::StoredLocalImage> AttachmentFetchWorkflow::StoreLocalImage(
    const std::string& thread_id, const ByteVector& plaintext, const std::string& mime) {
  const ByteVector dek_copy = CopyDek();
  if (dek_copy.empty() || profile_id_.empty()) {
    return Error("Storing an image requires unlocked DEK");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  if (auto saved = SaveAttachmentPlaintext(profile_dir_, profile_id_, *hash, mime, plaintext, std::string(), dek_copy);
      !saved) {
    return saved.error();
  }
  auto view = EnsureAttachmentView(profile_dir_, profile_id_, thread_id, *hash, mime, std::string(), dek_copy);
  if (!view) {
    return view.error();
  }
  return StoredLocalImage{.content_hash = std::move(*hash), .view_path = std::move(*view)};
}

namespace {

/** CAS object, RAM copy and this thread's view file of one object; errors are not reported (best effort). */
void DeleteLocalBytes(const std::string& profile_dir, const std::string& profile_id, const std::string& thread_id,
                      const std::vector<uint8_t>& content_hash, const std::string& mime, const std::string& filename) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return;
  }
  CasStore cas(profile_dir, profile_id);
  (void)cas.Delete(CasRealm::Private, content_hash); // the object id is the content hash (R016)
  AttachmentPlaintextMemoryCache::Instance().Erase(AttachmentHashHex(content_hash));
  std::error_code ec;
  if (const auto view = AttachmentLocalPath(profile_dir, thread_id, content_hash, mime, filename); !view.empty()) {
    std::filesystem::remove(view, ec);
  }
  std::filesystem::remove(AttachmentPosterPath(profile_dir, thread_id, content_hash), ec); // video first frame
}

std::vector<std::vector<uint8_t>> MessageContentHashes(const ThreadMessage& message) {
  std::vector<std::vector<uint8_t>> hashes;
  if (message.content_type == ChatContentType::Attachment) {
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json)) {
      hashes.push_back(fields->content_hash);
    }
  }
  if (message.ai_image_json) {
    if (const auto ref = AiImageRefFromJson(*message.ai_image_json)) {
      hashes.push_back(ref->content_hash);
    }
  }
  return hashes;
}

void DeleteFetchedBytes(const std::string& profile_dir, const std::string& profile_id, const std::string& thread_id,
                        const ChatAttachmentFields& fields) {
  DeleteLocalBytes(profile_dir, profile_id, thread_id, fields.content_hash, fields.mime, fields.filename);
}

/** The bytes a message refers to (a chat attachment, or the image of an AI question), unless still used. */
void DeleteMessageBytes(const std::string& profile_dir, const std::string& profile_id, const std::string& thread_id,
                        const ThreadMessage& message, const std::unordered_set<std::string>& still_used) {
  if (message.content_type == ChatContentType::Attachment) {
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
        fields && !still_used.contains(AttachmentHashHex(fields->content_hash))) {
      DeleteLocalBytes(profile_dir, profile_id, thread_id, fields->content_hash, fields->mime, fields->filename);
    }
  }
  if (message.ai_image_json) {
    if (const auto ref = AiImageRefFromJson(*message.ai_image_json);
        ref && !still_used.contains(AttachmentHashHex(ref->content_hash))) {
      DeleteLocalBytes(profile_dir, profile_id, thread_id, ref->content_hash, ref->mime, std::string());
    }
  }
}

} // namespace

Roe<void> AttachmentFetchWorkflow::DeleteThreadMedia(const std::string& thread_id, IThreadStore& store) {
  if (profile_dir_.empty() || profile_id_.empty()) {
    return Error("Deleting media requires a profile");
  }
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return page.error();
  }
  DeleteMessagesMedia(thread_id, *page, {});
  return {};
}

void AttachmentFetchWorkflow::DeleteMessagesMedia(const std::string& thread_id,
                                                  const std::vector<ThreadMessage>& messages,
                                                  const std::vector<ThreadMessage>& kept) {
  if (profile_dir_.empty() || profile_id_.empty()) {
    return;
  }
  // Content-addressed: bytes another live message of this thread still shows stay.
  std::unordered_set<std::string> still_used;
  for (const ThreadMessage& message : kept) {
    for (const std::vector<uint8_t>& hash : MessageContentHashes(message)) {
      still_used.insert(AttachmentHashHex(hash));
    }
  }
  std::unordered_set<std::string> keys;
  for (const ThreadMessage& message : messages) {
    if (message.content_type != ChatContentType::Attachment) {
      continue;
    }
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
        fields && !still_used.contains(AttachmentHashHex(fields->content_hash))) {
      keys.insert(JobKey(thread_id, fields->content_hash)); // a kept message's download goes on
    }
  }
  if (!keys.empty()) {
    std::lock_guard lock(mutex_);
    CancelJobsLocked([&keys](const std::string& key) { return keys.contains(key); });
  }
  for (const ThreadMessage& message : messages) {
    DeleteMessageBytes(profile_dir_, profile_id_, thread_id, message, still_used);
  }
}

void AttachmentFetchWorkflow::ForgetThread(const std::string& thread_id) {
  {
    std::lock_guard lock(mutex_);
    const std::string prefix = thread_id + "|";
    CancelJobsLocked([&prefix](const std::string& key) { return key.rfind(prefix, 0) == 0; });
  }
  if (suppression_) {
    (void)suppression_->ClearThread(thread_id);
  }
}

void AttachmentFetchWorkflow::CancelJobsLocked(const std::function<bool(const std::string& key)>& matches) {
  std::erase_if(queue_, [&](const Job& job) { return matches(JobKey(job)); });
  std::erase_if(active_, [&](const std::string& key) { return matches(key); });
  std::erase_if(pending_, [&](const std::string& key) { return matches(key); });
  std::erase_if(failed_, [&](const std::string& key) { return matches(key); });
  std::erase_if(throttled_, [&](const std::string& key) { return matches(key); });
}

void AttachmentFetchWorkflow::MaybeBuildPoster(const std::string& thread_id,
                                                 const ChatAttachmentFields& fields) {
  if (!IsAttachmentVideoMime(fields.mime) || profile_dir_.empty() || thread_id.empty()) {
    return;
  }
  const ByteVector dek_copy = CopyDek();
  if (dek_copy.empty() || profile_id_.empty()) {
    return;
  }
  (void)EnsureAttachmentPosterFromContent(profile_dir_, profile_id_, thread_id, fields.content_hash, fields.mime,
                                          fields.filename, dek_copy, fields.byte_length, Limits().max_receive_bytes);
}

void AttachmentFetchWorkflow::RetryDownload(const std::string& thread_id, const std::string& message_id,
                                              IThreadStore& store) {
  RequestDownload(thread_id, message_id, store);
}

void AttachmentFetchWorkflow::RequestDownload(const std::string& thread_id, const std::string& message_id,
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

void AttachmentFetchWorkflow::EnsureThreadQueued(const std::string& thread_id, IThreadStore& store) {
  auto page = store.GetMessagesPage(thread_id, std::nullopt, 10000);
  if (!page) {
    return;
  }
  for (const ThreadMessage& message : *page) {
    EnqueueFromMessage(thread_id, message);
  }
}

void AttachmentFetchWorkflow::DrainPendingMediaBacklog(IThreadStore& store) {
  const bool previous = backlog_drain_;
  backlog_drain_ = true;
  if (auto threads = store.ListThreads()) {
    for (const Thread& thread : *threads) {
      EnsureThreadQueued(thread.id, store);
    }
  }
  backlog_drain_ = previous;
}

Roe<void> AttachmentFetchWorkflow::PrepareThreadHistoryClear(const std::string& thread_id, IThreadStore& store) {
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
    const std::string prefix = thread_id + "|";
    CancelJobsLocked([&prefix](const std::string& key) { return key.rfind(prefix, 0) == 0; });
  }
  // Views only: the CAS bytes stay (messages keep them through "clear downloaded media"; clear history
  // deletes them itself once its rows are gone).
  return WipeThreadAttachmentBlobs(profile_dir_, thread_id);
}

Roe<void> AttachmentFetchWorkflow::ClearAllDownloadedMedia(IThreadStore& store) {
  auto threads = store.ListThreads();
  if (!threads) {
    return threads.error();
  }
  for (const Thread& thread : *threads) {
    if (auto cleared = PrepareThreadHistoryClear(thread.id, store); !cleared) {
      return cleared.error();
    }
  }
  NotifyChanged();
  return Roe<void>{};
}

} // namespace pbr
