#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/attachments/AttachmentContent.h"
#include "domain/attachments/AttachmentMetrics.h"
#include "common/Utilities.h"

#include "common/chat/MessagingLimits.h"
#include "foundation/crypto/AttachmentContentHash.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/runtime/BackgroundActivity.h"
#include "lib/messaging/AttachmentCache.h"
#include "lib/messaging/AttachmentPlaintextMemoryCache.h"
#include "lib/content/CasStore.h"
#include "lib/content/PrivateContent.h"
#include "lib/messaging/AiImageRef.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/voice/AudioFileTags.h"
#include "domain/attachments/AttachmentFetch.h"
#include "common/runtime/AppRuntime.h"
#include "common/ImageMetadata.h"

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <unordered_set>

namespace pbr {
namespace {

/** Storage-grid thumbnails and chat previews made at once (one limit for both). */
constexpr size_t kMaxGridThumbnailJobs = 2;

/** Content hashes a message refers to (attachment, AI image). */
std::vector<std::vector<uint8_t>> MessageContentHashes(const ThreadMessage& message);
/** Bytes a cancelled download wrote: CAS object, view, poster, RAM copy. */
void DeleteFetchedBytes(const std::string& profile_dir, CasStore& content, const std::string& thread_id,
                        const ChatAttachmentFields& fields);

} // namespace
} // namespace pbr
#include "common/PbrCompat.h"

namespace pbr {

void AttachmentFetchWorkflow::SetProfileDataDir(std::string profile_dir) {
  profile_dir_ = std::move(profile_dir);
}

void AttachmentFetchWorkflow::SetContent(std::shared_ptr<CasStore> content) {
  std::lock_guard lock(mutex_);
  content_ = std::move(content);
}

std::shared_ptr<CasStore> AttachmentFetchWorkflow::Content() const {
  std::lock_guard lock(mutex_);
  return content_;
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
  {
    std::lock_guard lock(mutex_);
    policy_ = policy;
  }
  KickDrain(); // queued jobs waiting for the network are judged again under the new policy
}

void AttachmentFetchWorkflow::SetAutoAllowedOnNetwork(const bool allowed) {
  std::vector<Job> again;
  {
    std::lock_guard lock(mutex_);
    network_allows_all_auto_ = allowed;
    if (allowed) {
      for (auto& [key, job] : waiting_for_network_) {
        pending_.erase(key);
        again.push_back(std::move(job));
      }
      waiting_for_network_.clear();
    }
  }
  // Back on a network that allows them: queued again, through the automatic limits like any new one.
  for (Job& job : again) {
    EnqueueJob(std::move(job), /*force=*/false);
  }
  KickDrain();
  if (!again.empty()) {
    NotifyChanged();
  }
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
uint64_t ScanAttachmentStoreBytes(const std::string& profile_dir, const CasStore* content) {
  if (profile_dir.empty()) {
    return 0;
  }
  uint64_t total = PendingAttachmentCiphertextBytes(profile_dir);
  if (content != nullptr) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(content->BlocksRoot(CasRealm::Private), ec), end;
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
                                            const uint64_t byte_length, const bool all_auto_pace) {
  AttachmentLimits limits;
  std::function<uint64_t()> provider;
  std::function<int64_t()> clock;
  std::function<uint64_t()> scan_override;
  bool need_scan = false;
  std::string profile_dir;
  std::shared_ptr<CasStore> content;
  {
    std::lock_guard lock(mutex_);
    limits = limits_;
    provider = disk_usage_;
    clock = clock_;
    scan_override = scan_override_;
    profile_dir = profile_dir_;
    content = content_;
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
      const uint64_t total = scan_override ? scan_override() : ScanAttachmentStoreBytes(profile_dir, content.get());
      std::lock_guard lock(mutex_);
      scanned_disk_bytes_ = total;
      scanned_at_ms_ = now_ms;
      started_since_scan_bytes_ = 0;
    }
    std::lock_guard lock(mutex_);
    used = scanned_disk_bytes_ + started_since_scan_bytes_;
  }
  // A large file stops at the lower mark, so a store filled by large files still takes the small ones.
  const uint64_t disk_cap = byte_length > kMaxDirectAttachmentBytes ? limits.auto_download_disk_cap_bytes
                                                                    : limits.auto_download_small_disk_cap_bytes;
  if (used >= disk_cap) {
    return false;
  }
  const uint64_t max_bytes =
      all_auto_pace ? limits.auto_download_all_max_bytes_per_minute : limits.auto_download_max_bytes_per_minute;
  if (!budget.TryAcquire(sender, byte_length, now_ms, limits.auto_download_max_count_per_minute, max_bytes)) {
    return false;
  }
  std::lock_guard lock(mutex_);
  started_since_scan_bytes_ += byte_length;
  return true;
}

bool AttachmentFetchWorkflow::TryAutoDownloadBudget(const std::string& sender, const uint64_t byte_length) {
  bool all_auto_pace = false;
  {
    std::lock_guard lock(mutex_);
    all_auto_pace = policy_ == AttachmentDownloadPolicy::AlwaysAuto && network_allows_all_auto_.value_or(false);
  }
  return AdmitByBudget(budget_, sender, byte_length, all_auto_pace);
}

bool AttachmentFetchWorkflow::TryAcceptDirectPush(const std::string& sender, const uint64_t ciphertext_bytes) {
  return AdmitByBudget(push_budget_, sender, ciphertext_bytes, /*all_auto_pace=*/false);
}

void AttachmentFetchWorkflow::RefundBudget(const Job& job) {
  if (!job.charged) {
    return;
  }
  budget_.Refund(job.sender, job.fields.byte_length);
  std::lock_guard lock(mutex_);
  started_since_scan_bytes_ -= std::min(started_since_scan_bytes_, job.fields.byte_length);
}

void AttachmentFetchWorkflow::SetOnChanged(ChangedCallback callback) {
  on_changed_ = std::move(callback);
}

void AttachmentFetchWorkflow::SetOnGridThumbnailReady(GridThumbnailCallback callback) {
  std::lock_guard lock(mutex_);
  on_grid_thumbnail_ready_ = std::move(callback);
}

void AttachmentFetchWorkflow::SetOnChatPreviewDone(ChatPreviewCallback callback) {
  std::lock_guard lock(mutex_);
  on_chat_preview_done_ = std::move(callback);
}

Roe<void> AttachmentFetchWorkflow::SetDek(ByteVector dek) {
  if (dek.size() != kDataEncryptionKeySize) {
    return Error("Invalid DEK size");
  }
  size_t grid_workers = 0;
  {
    std::lock_guard lock(mutex_);
    if (!dek_.empty()) {
      sodium_memzero(dek_.data(), dek_.size());
    }
    dek_ = std::move(dek);
    ++dek_generation_;
    grid_failed_.clear(); // a picture that failed may thumbnail now (another key, another hook state)
    grid_workers = ClaimGridWorkersLocked(); // the grid's jobs that waited while locked
  }
  for (size_t i = 0; i < grid_workers; ++i) {
    AppRuntime::PostWorkerBackground([this]() { RunGridThumbnails(); });
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
    ++dek_generation_;
    audio_tags_.clear(); // read from plaintext: forgotten with the view cache
    audio_tags_requested_.clear();
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

AttachmentFetchWorkflow::AutoVerdict AttachmentFetchWorkflow::AutoVerdictLocked(
    const ChatAttachmentFields& fields) const {
  const uint64_t bytes = fields.byte_length;
  if (bytes == 0 || bytes > limits_.max_receive_bytes) {
    return AutoVerdict::No; // an undeclared size is the sender's word for nothing: only a tap fetches it
  }
  if (policy_ == AttachmentDownloadPolicy::OnDemand) {
    // "Download nothing" saves data, but a voice message may be urgent and is small.
    return IsDeclaredVoiceMessage(fields.mime, fields.filename, bytes) ? AutoVerdict::Yes : AutoVerdict::No;
  }
  if (bytes <= kMaxDirectAttachmentBytes && ShouldAutoEnqueueAttachment(policy_, bytes, limits_.auto_download_max_bytes,
                                                                        /*network_allows_all_auto=*/false)) {
    return AutoVerdict::Yes; // the Smart rule, whatever the network
  }
  if (policy_ != AttachmentDownloadPolicy::AlwaysAuto) {
    return AutoVerdict::No;
  }
  if (bytes > kMaxDirectAttachmentBytes && fields.url.empty()) {
    return AutoVerdict::No; // a large file comes from the server only (the peer refuses it): no link, only a tap
  }
  if (!network_allows_all_auto_) {
    return AutoVerdict::NetworkUnknown;
  }
  return ShouldAutoEnqueueAttachment(policy_, bytes, limits_.auto_download_max_bytes, *network_allows_all_auto_)
             ? AutoVerdict::YesOnThisNetwork
             : AutoVerdict::NotOnThisNetwork;
}

AttachmentFetchWorkflow::AutoVerdict AttachmentFetchWorkflow::AutoVerdictFor(const ChatAttachmentFields& fields) const {
  std::lock_guard lock(mutex_);
  return AutoVerdictLocked(fields);
}

bool AttachmentFetchWorkflow::JobQueuedLocked(const std::string& key) const {
  return std::any_of(queue_.begin(), queue_.end(), [&](const Job& queued) { return JobKey(queued) == key; });
}

bool AttachmentFetchWorkflow::RejudgeLocked(const Job& job) const {
  return job.network_dependent && !forced_.contains(JobKey(job));
}

void AttachmentFetchWorkflow::HoldBackLocked(Job job, const AutoVerdict verdict) {
  const std::string key = JobKey(job);
  pending_.insert(key);
  if (verdict == AutoVerdict::NotOnThisNetwork) {
    job.charged = false; // given back by the caller
    waiting_for_network_.insert_or_assign(key, std::move(job));
  }
}

void AttachmentFetchWorkflow::KickDrain() {
  bool start = false;
  {
    std::lock_guard lock(mutex_);
    if (!queue_.empty() && !draining_) {
      draining_ = true;
      start = true;
    }
  }
  if (start) {
    AppRuntime::PostWorkerNormal([this]() { DrainQueue(); });
  }
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
  {
    std::lock_guard lock(mutex_);
    if (deleting_.contains(AttachmentHashHex(job.fields.content_hash))) {
      deferred_jobs_.emplace_back(std::move(job), force); // judged once the delete is done
      return;
    }
  }
  AttachmentFetchContext fetch_context;
  fetch_context.thread_id = job.thread_id;
  fetch_context.store = store_;
  fetch_context.peer_client = peer_client_;
  fetch_context.profile_data_dir = profile_dir_;
  if (!CanFetchAttachment(job.fields, fetch_context)) {
    return;
  }
  if (const auto content = Content(); content && AttachmentBlobExists(*content, job.fields.content_hash)) {
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
  job.network_dependent = false;
  job.charged = false;
  if (!force) {
    const AutoVerdict verdict = AutoVerdictFor(job.fields);
    if (verdict == AutoVerdict::No) {
      MarkPending(key);
      return;
    }
    if (verdict == AutoVerdict::NotOnThisNetwork) {
      {
        std::lock_guard lock(mutex_);
        if (active_.contains(key) || JobQueuedLocked(key)) {
          return; // queued before the network changed: judged again when it starts
        }
        if (!failed_.contains(key) && !throttled_.contains(key)) {
          waiting_for_network_.insert_or_assign(key, job); // comes by itself once the network allows it
        }
      }
      MarkPending(key);
      return;
    }
    job.network_dependent = verdict != AutoVerdict::Yes;
    bool already_throttled = false;
    {
      std::lock_guard lock(mutex_);
      // Already downloading or queued: a repeat look must not spend the sender's budget again.
      if (active_.contains(key) || JobQueuedLocked(key)) {
        return;
      }
      already_throttled = throttled_.contains(key);
    }
    // A sender over the automatic limits (or a full store) leaves the file as a "Download" row. A job waiting for
    // the network to be known is charged when it is judged, right before it starts.
    if (already_throttled ||
        (verdict != AutoVerdict::NetworkUnknown && !TryAutoDownloadBudget(job.sender, job.fields.byte_length))) {
      {
        std::lock_guard lock(mutex_);
        throttled_.insert(key);
      }
      MarkPending(key);
      return;
    }
    job.charged = verdict != AutoVerdict::NetworkUnknown;
  }

  bool should_start = false;
  bool duplicate = false;
  {
    std::lock_guard lock(mutex_);
    pending_.erase(key);
    throttled_.erase(key); // asked for (or allowed): no longer held back
    waiting_for_network_.erase(key);
    duplicate = active_.contains(key) || JobQueuedLocked(key);
    if (force) {
      // A tap: it starts whatever the network, also one queued by itself or waiting in the large-file lane (the
      // drain and the lane judge it again right before it starts, and skip the network for a forced one).
      forced_.insert(key);
    }
    if (duplicate && force) {
      should_start = !draining_;
      draining_ = true;
    }
    if (!duplicate) {
      failed_.erase(key);
      queue_.push_back(job);
      should_start = !draining_;
      draining_ = true;
    }
  }
  if (duplicate) {
    RefundBudget(job); // queued by another look meanwhile: this one spent nothing
  }
  if (should_start) {
    AppRuntime::PostWorkerNormal([this]() { DrainQueue(); });
  }
}

void AttachmentFetchWorkflow::DrainQueue() {
  std::optional<Job> next;
  bool next_rejudged = false;
  std::vector<Job> held_back;
  {
    std::lock_guard lock(mutex_);
    // The first job that may start now. One that depends on the network is judged again here (unless a tap forced
    // it): the network may have changed since it was queued; one waiting for the network to be known stays queued.
    for (auto it = queue_.begin(); it != queue_.end();) {
      const AutoVerdict verdict = RejudgeLocked(*it) ? AutoVerdictLocked(it->fields) : AutoVerdict::Yes;
      if (verdict == AutoVerdict::NetworkUnknown) {
        ++it;
        continue;
      }
      if (verdict == AutoVerdict::Yes || verdict == AutoVerdict::YesOnThisNetwork) {
        next = std::move(*it);
        queue_.erase(it);
        break;
      }
      held_back.push_back(*it);
      HoldBackLocked(std::move(*it), verdict);
      it = queue_.erase(it);
    }
    if (next) {
      active_.insert(JobKey(*next));
      next_rejudged = RejudgeLocked(*next);
    } else {
      draining_ = false;
    }
  }
  for (const Job& job : held_back) {
    RefundBudget(job);
  }
  if (!held_back.empty()) {
    NotifyChanged();
  }
  if (!next) {
    return;
  }
  Job job = std::move(*next);
  if (next_rejudged && !job.charged) {
    // Queued before any network was known, judged only now: the automatic limits apply now.
    if (!TryAutoDownloadBudget(job.sender, job.fields.byte_length)) {
      const std::string key = JobKey(job);
      {
        std::lock_guard lock(mutex_);
        active_.erase(key);
        throttled_.insert(key);
      }
      MarkPending(key);
      ContinueDrainOnWorker();
      return;
    }
    job.charged = true;
  }
  if (lane_ && job.fields.byte_length > kMaxDirectAttachmentBytes) {
    // A large download goes to its own lane (one at a time) and the drain goes on with the small ones: the
    // queue is not held for minutes by one big file.
    lane_->Post(LargeTransferLane::Direction::Download, [this, job = std::move(job)]() mutable {
      // It may have waited behind another large file: judged again now, unless a tap forced it meanwhile (a running
      // one is never stopped).
      bool start = true;
      {
        std::lock_guard lock(mutex_);
        const std::string key = JobKey(job);
        if (active_.contains(key) && RejudgeLocked(job)) {
          const AutoVerdict verdict = AutoVerdictLocked(job.fields);
          if (verdict != AutoVerdict::Yes && verdict != AutoVerdict::YesOnThisNetwork) {
            start = false;
            active_.erase(key);
            HoldBackLocked(job, verdict);
          }
        }
      }
      if (!start) {
        RefundBudget(job);
        NotifyChanged();
        return;
      }
      RunJobAsync(std::move(job), []() {});
    });
    ContinueDrainOnWorker();
    return;
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
  const int64_t started_ms = util::NowUnixMs();
  const uint64_t declared_bytes = job.fields.byte_length;
  AttachmentFetchContext context;
  context.thread_id = job.thread_id;
  context.store = store_;
  context.contacts = contacts_;
  context.identity = identity_;
  context.peer_client = peer_client_;
  context.profile_data_dir = profile_dir_;
  context.allow_local_cdn_for_test = allow_local_cdn_for_test_;
  // Percent of the CDN download for the card; a deleted message (no longer active) cancels the transfer.
  context.progress = [this, key](const uint64_t done, const uint64_t total) {
    if (lane_ && lane_->Stopping()) {
      return false; // the app is shutting down
    }
    {
      std::lock_guard lock(mutex_);
      if (!active_.contains(key)) {
        return false;
      }
    }
    SetProgress(key, total > 0 ? static_cast<int>(std::min<uint64_t>(100, done * 100 / total)) : 0);
    return true;
  };

  // A copy of the fields for the call: the callback below moves `job` away, and the arguments of one call are
  // evaluated before it runs (reading job.fields there found them already moved-from: no url, no hash).
  const ChatAttachmentFields fields = job.fields;
  // Download, decrypt and store, all of it, also if the app goes to the background meanwhile (released with the
  // last copy of the callbacks below).
  auto background = std::make_shared<BackgroundActivity>("attachment download");
  FetchAndDecryptAttachmentAsync(
      fields, context,
      [this, key, job = std::move(job), on_done = std::move(on_done), started_ms, declared_bytes,
       background](Roe<std::vector<uint8_t>> plaintext) mutable {
        auto finish_job = [this, key, job = std::move(job), on_done = std::move(on_done),
                           plaintext = std::move(plaintext), started_ms, declared_bytes,
                           background = std::move(background)]() mutable {
          // Reported at the end, after the bytes are stored, so the peak covers the whole receive.
          const auto report = [&](const bool ok) {
            EmitAttachmentTransferMetric("receive", declared_bytes, util::NowUnixMs() - started_ms, ok);
          };
          // First: a message deleted (or history cleared) meanwhile cancelled this job, and a cancelled transfer
          // that came back as an error is not a failure to show; its percent is gone with the job.
          bool cancelled = false;
          {
            std::lock_guard lock(mutex_);
            cancelled = !active_.contains(key);
            if (cancelled) {
              progress_.erase(key);
            }
          }
          if (cancelled) {
            if (on_done) {
              on_done();
            }
            return;
          }
          // Every answer of the relay spends one of the file's few downloads (expired, used up and deleted all look
          // the same 404): after one, no automatic retry asks again; the card's Download button does
          // (RetryDownload). A failure that never reached it (offline, no DNS, no connection) spent nothing.
          const auto hold_back_spent_download = [&] {
            if (suppression_ && !job.fields.url.empty()) {
              (void)suppression_->Suppress(job.thread_id, job.fields.content_hash);
            }
          };
          if (!plaintext) {
            report(false);
            if (FetchFailureReachedTheServer(plaintext.error())) {
              hold_back_spent_download();
            }
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          const ByteVector bytes = std::move(*plaintext); // no second whole-file copy
          const ByteVector dek_copy = CopyDek();
          const std::shared_ptr<CasStore> content = Content();
          if (dek_copy.empty() || !content) {
            report(false);
            hold_back_spent_download(); // the bytes came, so the download was spent
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          if (auto saved = SaveAttachmentPlaintext(*content, job.fields.content_hash, job.fields.mime, bytes,
                                                   job.fields.filename, dek_copy);
              !saved) {
            report(false);
            hold_back_spent_download();
            MarkFailed(key);
            if (on_done) {
              on_done();
            }
            return;
          }
          const uint64_t size_hint = job.fields.byte_length > 0 ? job.fields.byte_length : bytes.size();
          // A small file is ready to show at once. A large one is stored encrypted only: its plaintext view (and a
          // video's poster) is made when the person opens it, on a worker, and only for that session.
          if (size_hint <= kMaxDirectAttachmentBytes &&
              AttachmentAllowsInlinePrivateView(job.fields.mime, size_hint, Limits().max_receive_bytes)) {
            // From the bytes just decrypted, not read back from the store (that held the file two more times).
            (void)StoreAttachmentViewBytes(profile_dir_, job.thread_id, job.fields.content_hash, job.fields.mime,
                                           job.fields.filename, bytes);
          }
          MaybeBuildPoster(job.thread_id, job.fields);
          MaybeBuildImageThumbnail(job.thread_id, job.fields, bytes); // from the bytes in hand, no second decrypt
          // Cancelled while writing (message deleted): take back what was just written.
          bool cancelled_meanwhile = false;
          {
            std::lock_guard lock(mutex_);
            cancelled_meanwhile = !active_.contains(key);
          }
          if (cancelled_meanwhile) {
            DeleteFetchedBytes(profile_dir_, *content, job.thread_id, job.fields);
            if (on_done) {
              on_done();
            }
            return;
          }
          report(true);
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

void AttachmentFetchWorkflow::SetProgress(const std::string& key, const int percent) {
  {
    std::lock_guard lock(mutex_);
    // Every change redraws the chat; a large download would ask for a hundred. Five points at a time, and the end.
    auto& shown = progress_[key];
    if (shown == percent || (percent < 100 && percent - shown < 5)) {
      return;
    }
    shown = percent;
  }
  NotifyChanged();
}

int AttachmentFetchWorkflow::ProgressPercent(const std::string& thread_id,
                                             const std::vector<uint8_t>& content_hash) const {
  std::lock_guard lock(mutex_);
  const auto found = progress_.find(JobKey(thread_id, content_hash));
  return found == progress_.end() ? -1 : found->second;
}

void AttachmentFetchWorkflow::MarkFailed(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    progress_.erase(key);
    active_.erase(key);
    forced_.erase(key);
    pending_.erase(key);
    failed_.insert(key);
  }
  NotifyChanged();
}

void AttachmentFetchWorkflow::MarkReady(const std::string& key) {
  {
    std::lock_guard lock(mutex_);
    progress_.erase(key);
    active_.erase(key);
    forced_.erase(key);
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

void AttachmentFetchWorkflow::NotifyGridThumbnailReady(const std::string& thread_id,
                                                       const std::vector<uint8_t>& content_hash) {
  GridThumbnailCallback callback;
  {
    std::lock_guard lock(mutex_);
    callback = on_grid_thumbnail_ready_;
  }
  if (!callback) {
    return;
  }
  AppRuntime::PostUI(
      [callback = std::move(callback), thread_id, content_hash]() { callback(thread_id, content_hash); });
}

void AttachmentFetchWorkflow::NotifyChatPreviewDone(const std::string& thread_id,
                                                    const std::vector<uint8_t>& content_hash, const bool made) {
  ChatPreviewCallback callback;
  {
    std::lock_guard lock(mutex_);
    callback = on_chat_preview_done_;
  }
  if (!callback) {
    return;
  }
  AppRuntime::PostUI([callback = std::move(callback), thread_id, content_hash, made]() {
    callback(thread_id, content_hash, made);
  });
}

AttachmentFetchWorkflow::DownloadState AttachmentFetchWorkflow::StateFor(const std::string& thread_id,
                                                                         const ChatAttachmentFields& fields) const {
  const std::vector<uint8_t>& content_hash = fields.content_hash;
  const uint64_t byte_length = fields.byte_length;
  if (profile_dir_.empty() || thread_id.empty() || content_hash.size() != kAttachmentContentHashSize) {
    return DownloadState::Failed;
  }
  // Deleted from this chat (Me → Storage) while another chat keeps the bytes: "Download" here, not Ready.
  if (const auto content = Content();
      content && !SuppressedIn(thread_id, content_hash) && AttachmentBlobExists(*content, content_hash)) {
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
  // Not seen yet: what queueing it would do (the same rule). One waiting for the network to be known stays queued.
  switch (AutoVerdictLocked(fields)) {
    case AutoVerdict::Yes:
    case AutoVerdict::YesOnThisNetwork:
    case AutoVerdict::NetworkUnknown:
      return DownloadState::Downloading;
    case AutoVerdict::NotOnThisNetwork:
    case AutoVerdict::No:
    default:
      return DownloadState::Pending;
  }
}

Roe<std::string> AttachmentFetchWorkflow::EnsureLocalViewPath(const std::string& thread_id,
                                                              const std::vector<uint8_t>& content_hash,
                                                              const std::string& mime, const std::string& filename) {
  const ByteVector dek_copy = CopyDek();
  const std::shared_ptr<CasStore> content = Content();
  if (dek_copy.empty() || !content) {
    return Error("Attachment view requires unlocked DEK");
  }
  if (SuppressedIn(thread_id, content_hash)) {
    return Error("Attachment was deleted from this chat"); // another chat may still hold the bytes
  }
  return EnsureAttachmentView(profile_dir_, *content, thread_id, content_hash, mime, filename, dek_copy);
}

Roe<AttachmentFetchWorkflow::StoredLocalImage> AttachmentFetchWorkflow::StoreLocalImage(const std::string& thread_id,
                                                                                        const ByteVector& plaintext,
                                                                                        const std::string& mime) {
  const ByteVector dek_copy = CopyDek();
  const std::shared_ptr<CasStore> content = Content();
  if (dek_copy.empty() || !content) {
    return Error("Storing an image requires unlocked DEK");
  }
  auto hash = AttachmentContentHash(plaintext);
  if (!hash) {
    return hash.error();
  }
  AcquireBytesForSend(thread_id, *hash); // a delete of the same bytes running now keeps them (no wait)
  auto saved = SaveAttachmentPlaintext(*content, *hash, mime, plaintext, std::string(), dek_copy);
  ReleaseBytesForSend(*hash);
  if (!saved) {
    return saved.error();
  }
  auto view = EnsureAttachmentView(profile_dir_, *content, thread_id, *hash, mime, std::string(), dek_copy);
  if (!view) {
    return view.error();
  }
  return StoredLocalImage{.content_hash = std::move(*hash), .view_path = std::move(*view)};
}

namespace {

/** CAS object, RAM copy and this thread's view file of one object; errors are not reported (best effort). */
void DeleteLocalBytes(const std::string& profile_dir, CasStore& content, const std::string& thread_id,
                      const std::vector<uint8_t>& content_hash, const std::string& mime, const std::string& filename) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return;
  }
  (void)content.Delete(CasRealm::Private, content_hash); // the object id is the content hash (R016)
  AttachmentPlaintextMemoryCache::Instance().Erase(AttachmentHashHex(content_hash));
  std::error_code ec;
  if (const auto view = AttachmentLocalPath(profile_dir, thread_id, content_hash, mime, filename); !view.empty()) {
    std::filesystem::remove(view, ec);
  }
  std::filesystem::remove(AttachmentPosterPath(profile_dir, thread_id, content_hash), ec);      // video first frame
  std::filesystem::remove_all(AttachmentOpenLinkDir(profile_dir, thread_id, content_hash), ec); // its named link
}

/** What a message refers to in the private store: its attachment and / or the image of an AI question. */
struct MessageContentRef {
  std::vector<uint8_t> content_hash;
  std::string mime;
  std::string filename;
  bool has_url = false;
};

std::vector<MessageContentRef> MessageContentRefs(const ThreadMessage& message) {
  std::vector<MessageContentRef> refs;
  if (message.content_type == ChatContentType::Attachment) {
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json)) {
      refs.push_back(MessageContentRef{.content_hash = std::move(fields->content_hash),
                                       .mime = std::move(fields->mime),
                                       .filename = std::move(fields->filename),
                                       .has_url = !fields->url.empty()});
    }
  }
  if (message.ai_image_json) {
    if (auto ref = AiImageRefFromJson(*message.ai_image_json)) {
      refs.push_back(MessageContentRef{.content_hash = std::move(ref->content_hash), .mime = std::move(ref->mime)});
    }
  }
  return refs;
}

std::vector<std::vector<uint8_t>> MessageContentHashes(const ThreadMessage& message) {
  std::vector<std::vector<uint8_t>> hashes;
  for (MessageContentRef& ref : MessageContentRefs(message)) {
    hashes.push_back(std::move(ref.content_hash));
  }
  return hashes;
}

/** Messages read per page when a whole thread is scanned. */
constexpr size_t kScanPageSize = 500;

/**
 * Every message of a thread, newest first, read in pages to the end (no cap). false when a page cannot be read
 * (what was visited before stays visited).
 */
bool ForEachMessageNewestFirst(const IThreadStore& store, const std::string& thread_id,
                               const std::function<void(const ThreadMessage&)>& visit) {
  std::optional<int64_t> before;
  for (;;) {
    auto page = store.GetMessagesPage(thread_id, before, kScanPageSize);
    if (!page) {
      return false;
    }
    for (auto it = page->rbegin(); it != page->rend(); ++it) {
      visit(*it);
    }
    if (page->size() < kScanPageSize) {
      return true;
    }
    const int64_t oldest = page->front().display_order;
    if (before && oldest >= *before) {
      return true; // the order did not go down: nothing older to ask for
    }
    before = oldest;
  }
}

/** This thread's view files of one object (the view, the poster or thumbnail): their names start with its hex. */
void RemoveThreadViewsOf(const std::string& profile_dir, const std::string& thread_id, const std::string& hex) {
  std::error_code ec;
  const std::filesystem::path root(AttachmentViewRoot(profile_dir, thread_id));
  std::vector<std::filesystem::path> doomed;
  for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->path().filename().string().rfind(hex, 0) == 0) {
      doomed.push_back(it->path());
    }
  }
  for (const auto& path : doomed) {
    std::filesystem::remove(path, ec);
  }
}

void DeleteFetchedBytes(const std::string& profile_dir, CasStore& content, const std::string& thread_id,
                        const ChatAttachmentFields& fields) {
  DeleteLocalBytes(profile_dir, content, thread_id, fields.content_hash, fields.mime, fields.filename);
}

/** The bytes a message refers to (a chat attachment, or the image of an AI question), unless still used. */
void DeleteMessageBytes(const std::string& profile_dir, CasStore& content, const std::string& thread_id,
                        const ThreadMessage& message, const std::unordered_set<std::string>& still_used) {
  if (message.content_type == ChatContentType::Attachment) {
    if (auto fields = ChatPayloadCodec::DecodeAttachmentJson(message.payload_json);
        fields && !still_used.contains(AttachmentHashHex(fields->content_hash))) {
      DeleteLocalBytes(profile_dir, content, thread_id, fields->content_hash, fields->mime, fields->filename);
    }
  }
  if (message.ai_image_json) {
    if (const auto ref = AiImageRefFromJson(*message.ai_image_json);
        ref && !still_used.contains(AttachmentHashHex(ref->content_hash))) {
      DeleteLocalBytes(profile_dir, content, thread_id, ref->content_hash, ref->mime, std::string());
    }
  }
}

} // namespace

Roe<void> AttachmentFetchWorkflow::DeleteThreadMedia(const std::string& thread_id, IThreadStore& store) {
  if (profile_dir_.empty() || !Content()) {
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
  const std::shared_ptr<CasStore> content = Content();
  if (profile_dir_.empty() || !content) {
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
    DeleteMessageBytes(profile_dir_, *content, thread_id, message, still_used);
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
  std::erase_if(forced_, [&](const std::string& key) { return matches(key); });
  std::erase_if(pending_, [&](const std::string& key) { return matches(key); });
  std::erase_if(failed_, [&](const std::string& key) { return matches(key); });
  std::erase_if(throttled_, [&](const std::string& key) { return matches(key); });
  std::erase_if(waiting_for_network_, [&](const auto& entry) { return matches(entry.first); });
  std::erase_if(progress_, [&](const auto& entry) { return matches(entry.first); });
  // The chat's preview requests not started yet go with them (one being made sees the delete and takes it back).
  std::erase_if(chat_queue_, [&](const GridThumbnailJob& job) {
    const std::string key = JobKey(job.thread_id, job.content_hash);
    if (!matches(key)) {
      return false;
    }
    chat_preview_keys_.erase(key);
    return true;
  });
  std::erase_if(chat_preview_failed_, [&](const std::string& key) { return matches(key); });
}

void AttachmentFetchWorkflow::MaybeBuildPoster(const std::string& thread_id, const ChatAttachmentFields& fields,
                                               const bool after_open) {
  if (!IsAttachmentVideoMime(fields.mime) || profile_dir_.empty() || thread_id.empty()) {
    return;
  }
  if (fields.byte_length > kMaxDirectAttachmentBytes && !after_open) {
    return; // a large video gets its poster from the first open (that decrypts it for the session anyway)
  }
  const ByteVector dek_copy = CopyDek();
  const std::shared_ptr<CasStore> content = Content();
  if (dek_copy.empty() || !content) {
    return;
  }
  (void)EnsureAttachmentPosterFromContent(profile_dir_, *content, thread_id, fields.content_hash, fields.mime,
                                          fields.filename, dek_copy, fields.byte_length, Limits().max_receive_bytes);
}

void AttachmentFetchWorkflow::MaybeBuildImageThumbnail(const std::string& thread_id, const ChatAttachmentFields& fields,
                                                       const ByteVector& plaintext) {
  if (!IsAttachmentImageMime(fields.mime) || profile_dir_.empty() || thread_id.empty()) {
    return;
  }
  if (fields.byte_length <= kMaxDirectAttachmentBytes) {
    // Its size is read from the bytes in hand: a picture no longer than the preview is shown from its own view (and
    // one whose head cannot be read is a file card, never decoded for a bubble).
    const auto header = ReadImageHeaderBytes(plaintext);
    if (!header || !ImageNeedsPreview(header->width, header->height)) {
      return;
    }
  }
  if (!HasDek()) {
    return; // locked: nothing derived from the plaintext is kept on disk
  }
  (void)EnsureAttachmentImageThumbnail(profile_dir_, thread_id, fields.content_hash, fields.mime, plaintext);
}

void AttachmentFetchWorkflow::RequestChatPreview(const std::string& thread_id, const ChatAttachmentFields& fields) {
  if (!IsAttachmentImageMime(fields.mime) || profile_dir_.empty() || thread_id.empty() ||
      fields.content_hash.size() != kAttachmentContentHashSize || !AppRuntime::IsRunning()) {
    return; // never on the caller's (render) thread
  }
  const std::string key = JobKey(thread_id, fields.content_hash);
  size_t workers = 0;
  {
    std::lock_guard lock(mutex_);
    if (!content_ || chat_preview_failed_.contains(key) || deleting_.contains(AttachmentHashHex(fields.content_hash))) {
      return;
    }
    const auto queued = std::find_if(chat_queue_.begin(), chat_queue_.end(), [&](const GridThumbnailJob& job) {
      return JobKey(job.thread_id, job.content_hash) == key;
    });
    if (queued != chat_queue_.end()) {
      // Asked again (the rows drawn again, oldest first): it goes to the front with the others in their new order.
      GridThumbnailJob job = std::move(*queued);
      chat_queue_.erase(queued);
      chat_queue_.push_back(std::move(job));
      return;
    }
    if (!chat_preview_keys_.insert(key).second) {
      return; // being made
    }
    chat_queue_.push_back(GridThumbnailJob{.thread_id = thread_id, .content_hash = fields.content_hash, .mime = fields.mime});
    workers = ClaimGridWorkersLocked(); // none while locked: the job waits for SetDek
  }
  for (size_t i = 0; i < workers; ++i) {
    AppRuntime::PostWorkerBackground([this]() { RunGridThumbnails(); });
  }
}

void AttachmentFetchWorkflow::CancelChatPreviews() {
  std::lock_guard lock(mutex_);
  for (const GridThumbnailJob& job : chat_queue_) {
    chat_preview_keys_.erase(JobKey(job.thread_id, job.content_hash));
  }
  chat_queue_.clear();
}

bool AttachmentFetchWorkflow::ChatPreviewFailed(const std::string& thread_id,
                                                const std::vector<uint8_t>& content_hash) const {
  std::lock_guard lock(mutex_);
  return chat_preview_failed_.contains(JobKey(thread_id, content_hash));
}

std::optional<AttachmentFetchWorkflow::AudioCardTags> AttachmentFetchWorkflow::AudioTags(
    const std::string& thread_id, const std::vector<uint8_t>& content_hash) const {
  std::lock_guard lock(mutex_);
  const auto found = audio_tags_.find(JobKey(thread_id, content_hash));
  if (found == audio_tags_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void AttachmentFetchWorkflow::RequestAudioTags(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                               const std::string& view_path) {
  if (profile_dir_.empty() || thread_id.empty() || view_path.empty() ||
      content_hash.size() != kAttachmentContentHashSize || !AppRuntime::IsRunning()) {
    return; // never on the caller's (render) thread
  }
  const std::string key = JobKey(thread_id, content_hash);
  const std::string hex = AttachmentHashHex(content_hash);
  {
    std::lock_guard lock(mutex_);
    if (dek_.empty() || deleting_.contains(hex) || !audio_tags_requested_.insert(key).second) {
      return; // locked, being deleted, or read (or being read) already
    }
    ++thumbnails_in_progress_[hex]; // a delete of these bytes waits for the cover
  }
  AppRuntime::PostWorkerBackground([this, thread_id, content_hash, key, hex, view_path]() {
    // No lock held: the system's reader parses the file. What it finds stays on this device.
    std::optional<AudioFileTags> read = ReadAudioFileTags(view_path);
    AudioCardTags tags;
    bool cover = false;
    if (read) {
      tags = {.title = std::move(read->title), .artist = std::move(read->artist), .duration_ms = read->duration_ms};
      if (!read->picture.empty() && HasDek()) {
        const ByteVector picture(read->picture.begin(), read->picture.end());
        cover = static_cast<bool>(EnsureAttachmentAudioCover(profile_dir_, thread_id, content_hash, picture));
      }
    }
    {
      std::lock_guard lock(mutex_);
      if (dek_.empty() || deleting_.contains(hex)) {
        // Locked (its view cache wiped) or deleted meanwhile: nothing read from it stays.
        if (cover) {
          std::error_code ec;
          std::filesystem::remove(AttachmentPosterPath(profile_dir_, thread_id, content_hash), ec);
        }
        audio_tags_requested_.erase(key);
      } else {
        audio_tags_[key] = std::move(tags);
      }
      if (--thumbnails_in_progress_[hex] <= 0) {
        thumbnails_in_progress_.erase(hex);
      }
    }
    delete_cv_.notify_all();
    NotifyChanged(); // the card re-renders with its title and cover
  });
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
    {
      std::lock_guard lock(mutex_);
      if (deleting_.contains(AttachmentHashHex(fields->content_hash))) {
        // Asked for while its bytes are being deleted: done right after, so the delete cannot suppress it again.
        deferred_requests_.push_back(
            DeferredRequest{.thread_id = thread_id, .message_id = message_id, .store = &store});
        return;
      }
    }
    if (suppression_) {
      (void)suppression_->ClearSuppression(thread_id, fields->content_hash);
    }
    if (const auto content = Content(); content && AttachmentBlobExists(*content, fields->content_hash)) {
      NotifyChanged(); // deleted here but kept for another chat: shown again at once, nothing to fetch
      return;
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

bool AttachmentFetchWorkflow::SuppressedIn(const std::string& thread_id,
                                           const std::vector<uint8_t>& content_hash) const {
  return suppression_ != nullptr && suppression_->IsSuppressed(thread_id, content_hash);
}

std::vector<StoredAttachmentItem> AttachmentFetchWorkflow::ListStoredAttachments(IThreadStore& store) {
  std::vector<StoredAttachmentItem> items;
  const std::shared_ptr<CasStore> content = Content();
  if (profile_dir_.empty() || !content) {
    return items;
  }
  auto threads = store.ListThreads();
  if (!threads) {
    return items;
  }
  std::unordered_map<std::string, uint64_t> stored_bytes; // hex → size on disk (0: not stored), looked up once
  for (const Thread& thread : *threads) {
    std::unordered_set<std::string> seen; // per chat: the newest message of a hash stands for it
    (void)ForEachMessageNewestFirst(store, thread.id, [&](const ThreadMessage& message) {
      for (MessageContentRef& ref : MessageContentRefs(message)) {
        if (ref.content_hash.size() != kAttachmentContentHashSize) {
          continue;
        }
        const std::string hex = AttachmentHashHex(ref.content_hash);
        if (!seen.insert(hex).second) {
          continue;
        }
        auto found = stored_bytes.find(hex);
        if (found == stored_bytes.end()) {
          found = stored_bytes.emplace(hex, AttachmentLocalBytes(*content, ref.content_hash)).first;
        }
        if (found->second == 0 || SuppressedIn(thread.id, ref.content_hash)) {
          continue; // not on this device, or deleted from this chat
        }
        items.push_back(StoredAttachmentItem{.thread_id = thread.id,
                                             .message_id = message.id,
                                             .content_hash = std::move(ref.content_hash),
                                             .mime = std::move(ref.mime),
                                             .filename = std::move(ref.filename),
                                             .bytes = found->second,
                                             .timestamp_ms = message.timestamp,
                                             .has_server_copy = ref.has_url});
      }
    });
  }
  return items;
}

std::unordered_set<std::string> AttachmentFetchWorkflow::HashesUsedElsewhere(
    IThreadStore& store, const std::unordered_set<std::string>& items, const std::unordered_set<std::string>& targets) {
  std::unordered_set<std::string> used;
  {
    // First the sends in flight: one that ends after this look has written its message, which the scan below sees.
    std::lock_guard lock(mutex_);
    for (const std::string& hex : targets) {
      if (bytes_in_use_.contains(hex)) {
        used.insert(hex);
      }
    }
  }
  auto threads = store.ListThreads();
  if (!threads) {
    return targets; // not known: nothing is freed
  }
  for (const Thread& thread : *threads) {
    const bool read = ForEachMessageNewestFirst(store, thread.id, [&](const ThreadMessage& message) {
      for (const MessageContentRef& ref : MessageContentRefs(message)) {
        const std::string hex = AttachmentHashHex(ref.content_hash);
        if (!targets.contains(hex) || used.contains(hex) || items.contains(JobKey(thread.id, ref.content_hash))) {
          continue;
        }
        if (SuppressedIn(thread.id, ref.content_hash)) {
          continue; // deleted from that chat earlier: it does not keep the bytes
        }
        used.insert(hex);
      }
    });
    if (!read) {
      return targets;
    }
  }
  return used;
}

uint64_t AttachmentFetchWorkflow::ReleasableBytes(IThreadStore& store, const std::vector<StoredAttachmentRef>& items) {
  const std::shared_ptr<CasStore> content = Content();
  if (profile_dir_.empty() || !content) {
    return 0;
  }
  std::unordered_set<std::string> keys;
  std::unordered_map<std::string, std::vector<uint8_t>> targets; // hex → hash
  for (const auto& [thread_id, hash] : items) {
    if (thread_id.empty() || hash.size() != kAttachmentContentHashSize) {
      continue;
    }
    keys.insert(JobKey(thread_id, hash));
    targets.emplace(AttachmentHashHex(hash), hash);
  }
  std::unordered_set<std::string> target_hex;
  for (const auto& [hex, hash] : targets) {
    target_hex.insert(hex);
  }
  const std::unordered_set<std::string> used = HashesUsedElsewhere(store, keys, target_hex);
  uint64_t total = 0;
  for (const auto& [hex, hash] : targets) {
    if (!used.contains(hex)) {
      total += AttachmentLocalBytes(*content, hash);
    }
  }
  return total;
}

void AttachmentFetchWorkflow::DeleteLocalAttachments(IThreadStore& store, std::vector<StoredAttachmentRef> items,
                                                     std::function<void(StoredAttachmentDeleteResult)> done) {
  StoredAttachmentDeleteResult result;
  const auto finish = [&]() {
    if (done) {
      done(result);
    }
  };
  const std::shared_ptr<CasStore> content = Content();
  if (profile_dir_.empty() || !content) {
    finish();
    return;
  }
  std::lock_guard serial(delete_serial_);

  std::unordered_set<std::string> keys;                          // "thread|hex"
  std::unordered_map<std::string, std::vector<uint8_t>> targets; // hex → hash
  std::unordered_map<std::string, std::vector<std::vector<uint8_t>>> by_thread;
  for (auto& [thread_id, hash] : items) {
    if (thread_id.empty() || hash.size() != kAttachmentContentHashSize ||
        !keys.insert(JobKey(thread_id, hash)).second) {
      continue;
    }
    targets.emplace(AttachmentHashHex(hash), hash);
    by_thread[thread_id].push_back(std::move(hash));
  }
  if (keys.empty()) {
    finish();
    return;
  }
  std::unordered_set<std::string> target_hex;
  for (const auto& [hex, hash] : targets) {
    target_hex.insert(hex);
  }

  // 1. Mark: downloads and sends of these bytes wait from here; their downloads and not-started thumbnails go, and a
  //    thumbnail being made is waited for (it sees the mark and takes its file back).
  {
    std::unique_lock lock(mutex_);
    deleting_.insert(target_hex.begin(), target_hex.end());
    CancelJobsLocked([&keys](const std::string& key) { return keys.contains(key); });
    std::erase_if(grid_queue_, [&](const GridThumbnailJob& job) {
      return target_hex.contains(AttachmentHashHex(job.content_hash));
    });
    std::erase_if(chat_queue_, [&](const GridThumbnailJob& job) {
      if (!target_hex.contains(AttachmentHashHex(job.content_hash))) {
        return false;
      }
      chat_preview_keys_.erase(JobKey(job.thread_id, job.content_hash));
      return true;
    });
    std::erase_if(grid_failed_, [&keys](const std::string& key) { return keys.contains(key); });
    std::erase_if(audio_tags_, [&keys](const auto& entry) { return keys.contains(entry.first); });
    std::erase_if(audio_tags_requested_, [&keys](const std::string& key) { return keys.contains(key); });
    delete_cv_.wait(lock, [&] {
      return std::none_of(target_hex.begin(), target_hex.end(),
                          [&](const std::string& hex) { return thumbnails_in_progress_.contains(hex); });
    });
  }

  // 2. Who else uses them: every chat's messages (AI images too) and the sends in flight; the items themselves not.
  //    A message with the same bytes arriving in another chat after this scan is not seen (accepted: rare; it is
  //    downloaded again by the usual rules).
  std::unordered_set<std::string> kept = HashesUsedElsewhere(store, keys, target_hex);

  // 3. The bytes of a hash nothing else uses deleted. Checked and deleted under the lock: a send or an AI image
  //    storing the same bytes (AcquireBytesForSend, which never waits) either came first and keeps them, or comes
  //    after and writes them again.
  for (const auto& [hex, hash] : targets) {
    if (kept.contains(hex)) {
      continue;
    }
    const uint64_t bytes = AttachmentLocalBytes(*content, hash);
    {
      std::lock_guard lock(mutex_);
      const bool sent_meanwhile =
          bytes_in_use_.contains(hex) ||
          std::any_of(sent_during_delete_.begin(), sent_during_delete_.end(),
                      [&](const StoredAttachmentRef& sent) { return AttachmentHashHex(sent.second) == hex; });
      if (sent_meanwhile) {
        kept.insert(hex);
        continue;
      }
      (void)content->Delete(CasRealm::Private, ByteVector(hash.begin(), hash.end())); // the object id is the hash
      AttachmentPlaintextMemoryCache::Instance().Erase(hex);
      AttachmentPlaintextMemoryCache::Instance().Erase(hex + ".poster.jpg");
    }
    if (!AttachmentBlobExists(*content, hash)) {
      result.freed_bytes += bytes;
    }
  }
  //    Off each chat for good (on disk: no automatic download brings it back), its views gone.
  for (const auto& [thread_id, hashes] : by_thread) {
    if (suppression_) {
      (void)suppression_->SuppressAll(thread_id, hashes);
    }
    for (const std::vector<uint8_t>& hash : hashes) {
      RemoveThreadViewsOf(profile_dir_, thread_id, AttachmentHashHex(hash));
      ++result.deleted;
      if (kept.contains(AttachmentHashHex(hash))) {
        ++result.kept_shared;
      }
    }
  }

  // 4. Unmark; what waited goes on (judged now, by the usual rules).
  std::vector<std::pair<Job, bool>> jobs;
  std::vector<DeferredRequest> requests;
  std::vector<StoredAttachmentRef> sent;
  {
    std::lock_guard lock(mutex_);
    for (const std::string& hex : target_hex) {
      deleting_.erase(hex);
    }
    jobs.swap(deferred_jobs_);
    requests.swap(deferred_requests_);
    sent.swap(sent_during_delete_);
  }
  // Sent (or stored) into a chat while the delete ran: shown there again, though step 3 suppressed it.
  for (const auto& [thread_id, hash] : sent) {
    if (suppression_ && !thread_id.empty()) {
      (void)suppression_->ClearSuppression(thread_id, hash);
    }
  }
  for (const DeferredRequest& request : requests) {
    RequestDownload(request.thread_id, request.message_id, request.store != nullptr ? *request.store : store);
  }
  for (auto& [job, force] : jobs) {
    EnqueueJob(std::move(job), force);
  }
  NotifyChanged();
  finish();
}

void AttachmentFetchWorkflow::AcquireBytesForSend(const std::string& thread_id,
                                                  const std::vector<uint8_t>& content_hash) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return;
  }
  const std::string hex = AttachmentHashHex(content_hash);
  {
    // Never waits (an AI image is stored from whatever thread asks): a delete of these bytes running now sees
    // this under the same lock and keeps them, also after the release.
    std::lock_guard lock(mutex_);
    ++bytes_in_use_[hex];
    if (deleting_.contains(hex)) {
      sent_during_delete_.emplace_back(thread_id, content_hash);
    }
  }
  if (suppression_ && !thread_id.empty()) {
    (void)suppression_->ClearSuppression(thread_id, content_hash); // sent here again: shown here again
  }
}

void AttachmentFetchWorkflow::ReleaseBytesForSend(const std::vector<uint8_t>& content_hash) {
  if (content_hash.size() != kAttachmentContentHashSize) {
    return;
  }
  const std::string hex = AttachmentHashHex(content_hash);
  std::lock_guard lock(mutex_);
  const auto found = bytes_in_use_.find(hex);
  if (found != bytes_in_use_.end() && --found->second <= 0) {
    bytes_in_use_.erase(found);
  }
}

void AttachmentFetchWorkflow::RequestGridThumbnail(const std::string& thread_id,
                                                   const std::vector<uint8_t>& content_hash, const std::string& mime,
                                                   const uint64_t page_generation) {
  if (!IsAttachmentImageMime(mime) || profile_dir_.empty() || thread_id.empty() ||
      content_hash.size() != kAttachmentContentHashSize || !AppRuntime::IsRunning()) {
    return; // a video shows its poster when the chat made one, else a plain cell: never decrypted for the grid
  }
  if (AttachmentPosterExists(profile_dir_, thread_id, content_hash) || SuppressedIn(thread_id, content_hash)) {
    return;
  }
  const std::string key = JobKey(thread_id, content_hash);
  size_t workers = 0;
  {
    std::lock_guard lock(mutex_);
    if (!content_ || deleting_.contains(AttachmentHashHex(content_hash)) || grid_failed_.contains(key)) {
      return;
    }
    const bool queued = std::any_of(grid_queue_.begin(), grid_queue_.end(), [&](const GridThumbnailJob& job) {
      return job.generation == page_generation && JobKey(job.thread_id, job.content_hash) == key;
    });
    if (queued) {
      return;
    }
    grid_queue_.push_back(GridThumbnailJob{
        .thread_id = thread_id, .content_hash = content_hash, .mime = mime, .generation = page_generation});
    workers = ClaimGridWorkersLocked(); // none while locked: the job waits for SetDek
  }
  for (size_t i = 0; i < workers; ++i) {
    AppRuntime::PostWorkerBackground([this]() { RunGridThumbnails(); });
  }
}

size_t AttachmentFetchWorkflow::ClaimGridWorkersLocked() {
  if (dek_.empty() || !AppRuntime::IsRunning()) {
    return 0;
  }
  const size_t free = grid_running_ < kMaxGridThumbnailJobs ? kMaxGridThumbnailJobs - grid_running_ : 0;
  const size_t claimed = std::min(free, grid_queue_.size() + chat_queue_.size());
  grid_running_ += claimed;
  return claimed;
}

void AttachmentFetchWorkflow::CancelGridThumbnails(const uint64_t page_generation) {
  std::lock_guard lock(mutex_);
  std::erase_if(grid_queue_, [&](const GridThumbnailJob& job) { return job.generation == page_generation; });
}

std::string AttachmentFetchWorkflow::GridThumbnailPath(const std::string& thread_id,
                                                       const std::vector<uint8_t>& content_hash) const {
  if (profile_dir_.empty() || !AttachmentPosterExists(profile_dir_, thread_id, content_hash)) {
    return {};
  }
  return AttachmentPosterPath(profile_dir_, thread_id, content_hash);
}

void AttachmentFetchWorkflow::RunGridThumbnails() {
  for (;;) {
    GridThumbnailJob job;
    bool chat = false;
    std::string hex;
    ByteVector dek;
    uint64_t dek_generation = 0;
    {
      std::lock_guard lock(mutex_);
      if ((grid_queue_.empty() && chat_queue_.empty()) || dek_.empty()) {
        --grid_running_; // locked: the jobs left wait in the queues for SetDek
        return;
      }
      if (!chat_queue_.empty()) {
        // The chat first, its newest request first: those are the pictures on screen.
        job = std::move(chat_queue_.back());
        chat_queue_.pop_back();
        chat = true;
      } else {
        job = std::move(grid_queue_.front());
        grid_queue_.pop_front();
      }
      hex = AttachmentHashHex(job.content_hash);
      if (deleting_.contains(hex)) {
        if (chat) {
          chat_preview_keys_.erase(JobKey(job.thread_id, job.content_hash));
        }
        continue;
      }
      ++thumbnails_in_progress_[hex]; // started: a delete of these bytes waits for it
      dek = CopyDekUnlocked();
      dek_generation = dek_generation_;
    }
    const std::string key = JobKey(job.thread_id, job.content_hash);
    const std::shared_ptr<CasStore> content = Content();
    const bool exists_already = AttachmentPosterExists(profile_dir_, job.thread_id, job.content_hash);
    const bool attempt = content && AttachmentBlobExists(*content, job.content_hash) &&
                         !SuppressedIn(job.thread_id, job.content_hash) && !exists_already;
    // No lock held: decrypted once into memory (not through the plaintext memory cache: a large picture is not worth
    // keeping there), only the small JPEG is written (the preview's path).
    bool decrypted = false;
    Roe<std::string> made = Error("Nothing to thumbnail");
    if (attempt) {
      if (auto plain = LoadPrivateContent(*content, job.content_hash, dek)) {
        decrypted = true;
        made = EnsureAttachmentImageThumbnail(profile_dir_, job.thread_id, job.content_hash, job.mime, *plain);
      }
    }
    sodium_memzero(dek.data(), dek.size());
    bool notify = false;
    bool failed = false;
    {
      std::lock_guard lock(mutex_);
      const bool deleting = deleting_.contains(hex);
      // Locked (or unlocked with another key) while it ran: nothing made from it stays, and it is made again at the
      // next unlock; a decrypt that failed then says nothing about the picture.
      const bool key_changed = dek_generation_ != dek_generation;
      if (made) {
        if (key_changed || deleting || !AttachmentBlobExists(*content, job.content_hash)) {
          std::error_code ec;
          std::filesystem::remove(*made, ec);
        } else {
          notify = true;
        }
      } else if (decrypted && !key_changed && !deleting) {
        // The hook could not thumbnail valid bytes: not tried again (the grid's until the next unlock, the chat's this
        // run: its row becomes a file card).
        failed = true;
        (chat ? chat_preview_failed_ : grid_failed_).insert(key);
      } else if (chat && exists_already && !deleting && !key_changed) {
        notify = true; // made meanwhile (the download, the grid): the placeholder can take it
      }
      // A missing blob, a failed decrypt or a delete: not remembered, asked again when the row or cell shows again.
      const bool again = key_changed && !deleting && attempt && !notify;
      if (again) {
        if (chat) {
          chat_queue_.push_back(job);
        } else {
          grid_queue_.push_front(job);
        }
      } else if (chat) {
        chat_preview_keys_.erase(key);
      }
      if (--thumbnails_in_progress_[hex] <= 0) {
        thumbnails_in_progress_.erase(hex);
      }
    }
    delete_cv_.notify_all();
    if (chat && (notify || failed)) {
      NotifyChatPreviewDone(job.thread_id, job.content_hash, notify);
    } else if (notify) {
      NotifyGridThumbnailReady(job.thread_id, job.content_hash);
    }
  }
}

} // namespace pbr
