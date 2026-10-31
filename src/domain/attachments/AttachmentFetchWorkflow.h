#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "common/chat/AttachmentDownloadPolicy.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/messaging/AttachmentSuppressionStore.h"
#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/IThreadStore.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Background CDN fetch + decrypt for chat attachments (R008 / R021). */
class AttachmentFetchWorkflow : public IDekConsumer {
public:
  /** TooLarge: the declared size is above AttachmentLimits::max_receive_bytes; it is never downloaded. */
  enum class DownloadState { Ready, Pending, Downloading, Failed, TooLarge };

  using ChangedCallback = std::function<void()>;

  void SetProfileDataDir(std::string profile_dir);
  void SetProfileId(std::string profile_id);
  void SetFetchDependencies(IThreadStore* store, ContactsStore* contacts, IdentityStore* identity,
                              IChatBlobPeerClient* peer_client);
  void SetSuppressionStore(AttachmentSuppressionStore* suppression);
  void SetDownloadPolicy(AttachmentDownloadPolicy policy);
  void SetBacklogDrainActive(bool active);
  /** The receive-side limits (size, automatic-download pace and disk cap); default AttachmentLimits{}. */
  void SetLimits(const AttachmentLimits& limits);
  AttachmentLimits Limits() const;
  /** Bytes the attachment store holds (default: a scan of the private content blocks, cached for 30 s). */
  void SetDiskUsageProvider(std::function<uint64_t()> provider);
  /** Replaces the scan of the attachment store (tests only; it must run outside the workflow's lock). */
  void SetStoreScanForTest(std::function<uint64_t()> scan);
  /** Time source for the per-sender window (ms); tests only. */
  void SetClockForTest(std::function<int64_t()> now_ms);
  /**
   * Whether this sender may start one more download by itself now: false when the store is over the disk cap
   * or the sender is over its count / byte limit for the minute. A yes is recorded against the sender.
   */
  bool TryAutoDownloadBudget(const std::string& sender, uint64_t byte_length);
  /**
   * Whether a direct push from this sender may be kept: the same minute limits and disk cap as automatic
   * downloads (counted apart from them), over the store including pushed ciphertext not yet opened.
   */
  bool TryAcceptDirectPush(const std::string& sender, uint64_t ciphertext_bytes);
  void SetOnChanged(ChangedCallback callback);

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;
  bool HasDek() const;
  /** Copy of in-memory DEK under lock; empty when locked / unset. */
  ByteVector CopyDek() const;

  void EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message, bool force = false);
  void RequestDownload(const std::string& thread_id, const std::string& message_id, IThreadStore& store);
  void RetryDownload(const std::string& thread_id, const std::string& message_id, IThreadStore& store);

  /** Queue attachment messages not cached yet (respects policy unless backlog drain). */
  void EnsureThreadQueued(const std::string& thread_id, IThreadStore& store);
  void DrainPendingMediaBacklog(IThreadStore& store);

  /** Before clear-history: tombstone hashes and drop pending queue entries for the thread. */
  Roe<void> PrepareThreadHistoryClear(const std::string& thread_id, IThreadStore& store);

  /** Me → Storage: tombstone + wipe all thread attachment caches; messages stay (R020). */
  Roe<void> ClearAllDownloadedMedia(IThreadStore& store);

  /** Materialize plaintext view path for RmlUi / OS open while unlocked. */
  Roe<std::string> EnsureLocalViewPath(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                                       const std::string& mime, const std::string& filename);

  struct StoredLocalImage {
    std::vector<uint8_t> content_hash;
    std::string view_path;
  };
  /**
   * Keep an image this device produced (a question to the AI) like a received attachment: private CAS under
   * the profile DEK, plus the thread's blobs_view file for display. No wire fields, nothing uploaded.
   */
  Roe<StoredLocalImage> StoreLocalImage(const std::string& thread_id, const ByteVector& plaintext,
                                        const std::string& mime);
  /**
   * Remove the local bytes of this thread's media — attachment objects and AI images — from the private
   * CAS, the view files and the RAM cache: run before the thread or its history is deleted. Objects are
   * content-addressed, so identical bytes referenced from another thread are gone there too (an
   * attachment shows "download" again; an AI image shows its marker).
   */
  Roe<void> DeleteThreadMedia(const std::string& thread_id, IThreadStore& store);
  /**
   * The same for messages already read by the caller (one message on "delete for me", a whole thread
   * after its rows are gone). Their downloads in flight or queued are cancelled first, so a finishing
   * fetch cannot put the bytes back. Bytes that a message in `kept` (the rest of the thread) still refers
   * to stay. Identical bytes held by another thread are not checked: they show "download" again.
   */
  void DeleteMessagesMedia(const std::string& thread_id, const std::vector<ThreadMessage>& messages,
                           const std::vector<ThreadMessage>& kept);
  /** The thread is gone: cancel its downloads and drop its attachment suppression entries. */
  void ForgetThread(const std::string& thread_id);

  /** Best-effort video poster under blobs_view (R012); ignores errors. */
  void MaybeBuildPoster(const std::string& thread_id, const ChatAttachmentFields& fields);

  DownloadState StateFor(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                         uint64_t byte_length) const;
  /** Percent (0..100) of a download in progress; -1 when none is running or its size is not yet known. */
  int ProgressPercent(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  /** Test hook / UI: record a download's percent (notifies the transcript when it changes). */
  void SetProgress(const std::string& key, int percent);
  std::string JobKeyFor(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const {
    return JobKey(thread_id, content_hash);
  }

private:
  struct Job {
    std::string thread_id;
    std::string message_id;
    std::string sender;
    ChatAttachmentFields fields;
  };

  std::string JobKey(const Job& job) const;
  std::string JobKey(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  bool ShouldAutoDownload(const Job& job) const;
  void EnqueueJob(Job job, bool force);
  void DrainQueue();
  void RunJobAsync(Job job, std::function<void()> on_done);
  void MarkFailed(const std::string& key);
  void MarkReady(const std::string& key);
  /** Drop queued / in-flight jobs whose key matches; a finishing fetch for them then stores nothing. */
  void CancelJobsLocked(const std::function<bool(const std::string& key)>& matches);
  void MarkPending(const std::string& key);
  void NotifyChanged();
  bool AdmitByBudget(AutoDownloadBudget& budget, const std::string& sender, uint64_t byte_length);
  ByteVector CopyDekUnlocked() const;
  void ContinueDrainOnWorker();

  std::string profile_dir_;
  std::string profile_id_;
  ByteVector dek_;
  IThreadStore* store_ = nullptr;
  ContactsStore* contacts_ = nullptr;
  IdentityStore* identity_ = nullptr;
  IChatBlobPeerClient* peer_client_ = nullptr;
  AttachmentSuppressionStore* suppression_ = nullptr;
  AttachmentDownloadPolicy policy_ = AttachmentDownloadPolicy::Smart;
  bool backlog_drain_ = false;
  ChangedCallback on_changed_;
  mutable std::mutex mutex_;
  std::vector<Job> queue_;
  std::unordered_set<std::string> active_;
  std::unordered_set<std::string> failed_;
  std::unordered_set<std::string> pending_;
  /** Held back by the automatic-download limits: stays a "Download" row until the person taps it. */
  std::unordered_set<std::string> throttled_;
  std::unordered_map<std::string, int> progress_;
  AttachmentLimits limits_ = DefaultAttachmentLimits();
  AutoDownloadBudget budget_;
  AutoDownloadBudget push_budget_;
  std::function<uint64_t()> disk_usage_;
  std::function<int64_t()> clock_;
  std::function<uint64_t()> scan_override_;
  uint64_t scanned_disk_bytes_ = 0;
  int64_t scanned_at_ms_ = 0;
  uint64_t started_since_scan_bytes_ = 0;
  bool draining_ = false;
};

} // namespace pbr
