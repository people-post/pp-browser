#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "common/chat/AttachmentDownloadPolicy.h"
#include "domain/messaging/AttachmentSuppressionStore.h"
#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/IThreadStore.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Background CDN fetch + decrypt for chat attachments (R008 / R021). */
class AttachmentFetchWorkflow : public IDekConsumer {
public:
  enum class DownloadState { Ready, Pending, Downloading, Failed };

  using ChangedCallback = std::function<void()>;

  void SetProfileDataDir(std::string profile_dir);
  void SetProfileId(std::string profile_id);
  void SetFetchDependencies(IThreadStore* store, ContactsStore* contacts, IdentityStore* identity,
                              IChatBlobPeerClient* peer_client);
  void SetSuppressionStore(AttachmentSuppressionStore* suppression);
  void SetDownloadPolicy(AttachmentDownloadPolicy policy);
  void SetBacklogDrainActive(bool active);
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

private:
  struct Job {
    std::string thread_id;
    std::string message_id;
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
  bool draining_ = false;
};

} // namespace pbr
