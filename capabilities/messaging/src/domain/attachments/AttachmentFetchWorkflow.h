#pragma once

#include "lib/content/CasStore.h"
#include "lib/delivery/RelayClients.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IDekConsumer.h"
#include "lib/messaging/AttachmentDownloadPolicy.h"
#include "domain/attachments/LargeTransferLane.h"
#include "domain/attachments/StoredAttachments.h"
#include "lib/messaging/AttachmentLimits.h"
#include "lib/messaging/AttachmentSuppressionStore.h"
#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/IThreadStore.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"
#include "common/WorkerPool.h"

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
  using GridThumbnailCallback =
      std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash)>;
  /** A chat picture's preview (RequestChatPreview) is done: `made` true when it exists, false when it cannot be made. */
  using ChatPreviewCallback =
      std::function<void(const std::string& thread_id, const std::vector<uint8_t>& content_hash, bool made)>;

  AttachmentFetchWorkflow() = default;
  /** Drops the previews and grid thumbnails not started, waits for those being made and joins their threads. */
  ~AttachmentFetchWorkflow() override;
  AttachmentFetchWorkflow(const AttachmentFetchWorkflow&) = delete;
  AttachmentFetchWorkflow& operator=(const AttachmentFetchWorkflow&) = delete;

  void SetProfileDataDir(std::string profile_dir);
  /** The profile's content store (the hub's); none: nothing is stored or found. */
  void SetContent(std::shared_ptr<CasStore> content);
  void SetFetchDependencies(IThreadStore* store, ContactsStore* contacts, IdentityStore* identity,
                            IChatBlobPeerClient* peer_client);
  void SetSuppressionStore(AttachmentSuppressionStore* suppression);
  void SetDownloadPolicy(AttachmentDownloadPolicy policy);
  /**
   * Whether the current network is one "download everything" (AlwaysAuto) applies on; called once the network
   * is known and on every change of it or of the ticks. Until the first call, AlwaysAuto files above the Smart
   * size stay queued, not judged. Files held back only because the network did not allow them are queued again
   * (through the automatic limits) when it does.
   */
  void SetAutoAllowedOnNetwork(bool allowed);
  /** Where downloads above the direct size run (null: on the drain's own worker, as small ones do). */
  void SetTransferLane(LargeTransferLane* lane) { lane_ = lane; }
  /** The receive-side limits (size, automatic-download pace and disk cap); default AttachmentLimits{}. */
  void SetLimits(const AttachmentLimits& limits);
  AttachmentLimits Limits() const;
  /** Bytes the attachment store holds (default: a scan of the private content blocks, cached for 30 s). */
  void SetDiskUsageProvider(std::function<uint64_t()> provider);
  /** Replaces the scan of the attachment store (tests only; it must run outside the workflow's lock). */
  void SetStoreScanForTest(std::function<uint64_t()> scan);
  /** Time source for the per-sender window (ms); tests only. */
  void SetClockForTest(std::function<int64_t()> now_ms);
  /** Tests only: let a download use a plain-http loopback url. */
  void SetAllowLocalCdnForTest(bool allow) { allow_local_cdn_for_test_ = allow; }
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
  /** A storage-grid thumbnail (RequestGridThumbnail) was made; called on the UI thread instead of SetOnChanged's. */
  void SetOnGridThumbnailReady(GridThumbnailCallback callback);
  /** A chat preview (RequestChatPreview) was made or failed; called on the UI thread instead of SetOnChanged's. */
  void SetOnChatPreviewDone(ChatPreviewCallback callback);

  Roe<void> SetDek(ByteVector dek) override;
  void ClearDek() override;
  bool HasDek() const;
  /** Copy of in-memory DEK under lock; empty when locked / unset. */
  ByteVector CopyDek() const;

  void EnqueueFromMessage(const std::string& thread_id, const ThreadMessage& message, bool force = false);
  void RequestDownload(const std::string& thread_id, const std::string& message_id, IThreadStore& store);
  void RetryDownload(const std::string& thread_id, const std::string& message_id, IThreadStore& store);

  /** Queue attachment messages not cached yet (respects policy). */
  void EnsureThreadQueued(const std::string& thread_id, IThreadStore& store);

  /** Before clear-history: tombstone hashes and drop pending queue entries for the thread. */
  Roe<void> PrepareThreadHistoryClear(const std::string& thread_id, IThreadStore& store);

  /**
   * Me → Storage → Manage storage: every chat's attachments and AI images whose bytes this device holds, one per
   * (chat, hash), except those deleted from that chat (suppressed). Every message of every chat is read, in pages;
   * nothing is decrypted. `bytes` is the stored object's size on disk. Blocking: call on a worker.
   */
  std::vector<StoredAttachmentItem> ListStoredAttachments(IThreadStore& store);
  /**
   * The bytes deleting these (chat, hash) items would free: the stored size of each hash (counted once) that no
   * other message (of any chat, AI images included, a chat's suppressed items not counted) and no send in flight
   * still uses. Blocking: call on a worker.
   */
  uint64_t ReleasableBytes(IThreadStore& store, const std::vector<StoredAttachmentRef>& items);
  /**
   * Deletes these items from this device, in one go on the calling worker: (1) their hashes are marked as being
   * deleted (downloads asked for meanwhile wait), their downloads and not-started thumbnails are cancelled, and
   * thumbnails being made are waited for; (2) every chat's messages and the sends in flight are scanned for other
   * uses; (3) the bytes of a hash nothing else uses (nor a send or store that came during the delete) are deleted;
   * each item is suppressed in its chat (on disk: not downloaded again by itself, a tap brings it back) and its
   * views go; (4) the mark is cleared, what waited goes on, the change is notified and `done` is called (on this
   * thread) with the counts. One delete at a time.
   */
  void DeleteLocalAttachments(IThreadStore& store, std::vector<StoredAttachmentRef> items,
                              std::function<void(StoredAttachmentDeleteResult)> done);
  /**
   * A send of these bytes into `thread_id` (stored and its message written next): counts it as in use until
   * ReleaseBytesForSend, so a delete keeps the bytes. Never waits: a delete running now keeps them too, even after
   * the release. A send into a chat the item was deleted from shows it again (its suppression there is cleared).
   */
  void AcquireBytesForSend(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  void ReleaseBytesForSend(const std::vector<uint8_t>& content_hash);

  /**
   * A thumbnail for a cell of the storage grid: an image of any size, decrypted into memory on the previews' own
   * threads (not the shared worker pool) and made by the platform's thumbnail hook (the chat's large-image thumbnail,
   * same file). At most two at once; a video without a poster is not decrypted (nothing is made). Calls the grid-thumbnail callback when one is
   * made. Returns at once. While locked the jobs wait and start at the next unlock; a picture the hook cannot
   * thumbnail is not tried again until then. `page_generation` names the page that asked: CancelGridThumbnails
   * drops its jobs not started yet.
   */
  void RequestGridThumbnail(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                            const std::string& mime, uint64_t page_generation);
  void CancelGridThumbnails(uint64_t page_generation);
  /** The cell's thumbnail or video poster when one exists (empty: none). */
  std::string GridThumbnailPath(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;

  /** Materialize plaintext view path for RmlUi / OS open while unlocked (none for an item deleted from the chat). */
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
  void MaybeBuildPoster(const std::string& thread_id, const ChatAttachmentFields& fields, bool after_open = false);

  /**
   * Best-effort preview (kImagePreviewMaxSide) of a picture longer than that on its long side, or of any picture above
   * kMaxDirectAttachmentBytes (its size is not read without the plaintext): the chat bubble shows it, a tap opens the
   * original. Made from `plaintext` already in hand: call on a worker, not under a lock. A lock (or another unlock) or
   * a delete of the bytes while it is written takes it back, as RunGridThumbnails does.
   */
  void MaybeBuildImageThumbnail(const std::string& thread_id, const ChatAttachmentFields& fields,
                                const ByteVector& plaintext);
  /**
   * The chat shows a picture's placeholder (its preview is not there: the view cache is wiped at every start and lock):
   * the preview is made on the previews' own threads (decrypted once into memory), at most two at once together with
   * the storage grid's thumbnails, the latest request first (the rows are drawn oldest first: the last ones asked for
   * are on screen). Returns at once; asked again while queued, it moves to the front. The chat-preview callback says
   * when it is done. While locked the requests wait for the next unlock. One the platform cannot make from valid
   * bytes is not tried again this run (ChatPreviewFailed); one stopped by a lock, a delete or CancelChatPreviews is
   * asked again when its row shows again.
   */
  void RequestChatPreview(const std::string& thread_id, const ChatAttachmentFields& fields);
  /** The chat page was left: its preview requests not started yet are dropped (the storage grid's stay). */
  void CancelChatPreviews();
  /** Whether this picture's preview could not be made from its bytes this run (its row is a file card). */
  bool ChatPreviewFailed(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;

  /** What a sound file's card shows from its own tags (empty strings / 0 when the file names none). */
  struct AudioCardTags {
    std::string title;
    std::string artist;
    int64_t duration_ms = 0;
  };
  /** The tags read this session for these bytes; nullopt before RequestAudioTags has finished for them. */
  std::optional<AudioCardTags> AudioTags(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  /**
   * The transcript shows a sound file whose plaintext view `view_path` exists: reads its tags (ReadAudioFileTags)
   * on a background worker, its cover going to the poster path as a small JPEG (EnsureAttachmentAudioCover), then
   * notifies the change. Once per bytes a session (a lock forgets them with the view cache). Local only.
   */
  void RequestAudioTags(const std::string& thread_id, const std::vector<uint8_t>& content_hash,
                        const std::string& view_path);

  /** An item deleted from this chat (suppressed) is never Ready here, even while another chat keeps its bytes. */
  DownloadState StateFor(const std::string& thread_id, const ChatAttachmentFields& fields) const;
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
    /** Its start depends on the network allowing "download everything": judged again right before it starts. */
    bool network_dependent = false;
    /** The automatic limits were charged for it (given back when it does not start after all). */
    bool charged = false;
  };

  /** Whether an attachment starts by itself now (the one rule for queueing and for StateFor). */
  enum class AutoVerdict {
    Yes,
    /** Above the Smart size: only because the network allows "download everything". */
    YesOnThisNetwork,
    /** Above the Smart size and the network does not allow it: comes when it does. */
    NotOnThisNetwork,
    /** Above the Smart size and no network known yet: waits in the queue, not judged. */
    NetworkUnknown,
    No,
  };

  std::string JobKey(const Job& job) const;
  std::string JobKey(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  AutoVerdict AutoVerdictLocked(const ChatAttachmentFields& fields) const;
  AutoVerdict AutoVerdictFor(const ChatAttachmentFields& fields) const;
  bool JobQueuedLocked(const std::string& key) const;
  /** Whether a queued job is judged again before it starts: it depends on the network and no tap forced it. */
  bool RejudgeLocked(const Job& job) const;
  /** Gives back what the automatic limits recorded for a job that did not start after all. */
  void RefundBudget(const Job& job);
  /** A queued job does not start after all (the network or the policy changed): it becomes a Download row. */
  void HoldBackLocked(Job job, AutoVerdict verdict);
  /** Starts the drain when jobs are queued and none runs (queued jobs may have become startable). */
  void KickDrain();
  void EnqueueJob(Job job, bool force);
  void DrainQueue();
  void RunJobAsync(Job job, std::function<void()> on_done);
  void MarkFailed(const std::string& key);
  void MarkReady(const std::string& key);
  /** Drop queued / in-flight jobs whose key matches; a finishing fetch for them then stores nothing. */
  void CancelJobsLocked(const std::function<bool(const std::string& key)>& matches);
  void MarkPending(const std::string& key);
  void NotifyChanged();
  void NotifyGridThumbnailReady(const std::string& thread_id, const std::vector<uint8_t>& content_hash);
  void NotifyChatPreviewDone(const std::string& thread_id, const std::vector<uint8_t>& content_hash, bool made);
  /**
   * Under mutex_: claims workers for queued grid and chat jobs (at most two run), starting the previews' own threads
   * at the first claim; the caller then posts that many (PostGridWorkers).
   */
  size_t ClaimGridWorkersLocked();
  /** `count` workers just claimed, each a RunGridThumbnails on the previews' own threads. */
  void PostGridWorkers(size_t count);
  /** `all_auto_pace`: "download everything" on an allowed network (the wider per-minute byte limit). */
  bool AdmitByBudget(AutoDownloadBudget& budget, const std::string& sender, uint64_t byte_length, bool all_auto_pace);
  ByteVector CopyDekUnlocked() const;
  struct GridThumbnailJob {
    std::string thread_id;
    std::vector<uint8_t> content_hash;
    std::string mime;
    uint64_t generation = 0;
  };
  /** A worker's loop: the chat's previews (newest first), then the grid's thumbnails (in order), until none is left. */
  void RunGridThumbnails();
  /** Hex of the hashes in `targets` that something other than `items` uses (messages, sends in flight). */
  std::unordered_set<std::string> HashesUsedElsewhere(IThreadStore& store, const std::unordered_set<std::string>& items,
                                                      const std::unordered_set<std::string>& targets);
  bool SuppressedIn(const std::string& thread_id, const std::vector<uint8_t>& content_hash) const;
  std::shared_ptr<CasStore> Content() const;
  void ContinueDrainOnWorker();

  std::string profile_dir_;
  std::shared_ptr<CasStore> content_;
  ByteVector dek_;
  IThreadStore* store_ = nullptr;
  ContactsStore* contacts_ = nullptr;
  IdentityStore* identity_ = nullptr;
  IChatBlobPeerClient* peer_client_ = nullptr;
  AttachmentSuppressionStore* suppression_ = nullptr;
  AttachmentDownloadPolicy policy_ = AttachmentDownloadPolicy::Smart; // under mutex_
  /** Under mutex_. Whether the network allows "download everything"; none: not known yet. */
  std::optional<bool> network_allows_all_auto_;
  ChangedCallback on_changed_;
  /** Under mutex_. */
  GridThumbnailCallback on_grid_thumbnail_ready_;
  /** Under mutex_. */
  ChatPreviewCallback on_chat_preview_done_;
  /** Under mutex_: bumped by SetDek / ClearDek (a grid job tells a lock or a new key during its run). */
  uint64_t dek_generation_ = 0;
  mutable std::mutex mutex_;
  std::vector<Job> queue_;
  std::unordered_set<std::string> active_;
  std::unordered_set<std::string> failed_;
  std::unordered_set<std::string> pending_;
  /** Held back by the automatic-download limits: stays a "Download" row until the person taps it. */
  std::unordered_set<std::string> throttled_;
  /** Held back only because the current network does not allow it (also in pending_): queued when it does. */
  std::unordered_map<std::string, Job> waiting_for_network_;
  /** Jobs a tap asked for (queued, in the lane or running): started whatever the network; gone when they finish. */
  std::unordered_set<std::string> forced_;
  std::unordered_map<std::string, int> progress_;
  LargeTransferLane* lane_ = nullptr;
  bool allow_local_cdn_for_test_ = false;
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
  /** Under mutex_: hex of the hashes a DeleteLocalAttachments is deleting. */
  std::unordered_set<std::string> deleting_;
  /** Under mutex_: downloads asked for while their hash was being deleted, run once it is done. */
  std::vector<std::pair<Job, bool>> deferred_jobs_;
  struct DeferredRequest {
    std::string thread_id;
    std::string message_id;
    IThreadStore* store = nullptr;
  };
  std::vector<DeferredRequest> deferred_requests_;
  /** Under mutex_: hex → sends in flight using those bytes (AcquireBytesForSend). */
  std::unordered_map<std::string, int> bytes_in_use_;
  /** Under mutex_: (chat, hash) sent or stored while that hash was being deleted: the delete keeps them. */
  std::vector<StoredAttachmentRef> sent_during_delete_;
  /** Under mutex_: job key (chat + bytes) → a sound file's tags read this session; the keys asked for (read or being
   *  read). */
  std::unordered_map<std::string, AudioCardTags> audio_tags_;
  std::unordered_set<std::string> audio_tags_requested_;
  /** Under mutex_: hex → thumbnails being made from those bytes (a delete waits for them). */
  std::unordered_map<std::string, int> thumbnails_in_progress_;
  /** Signalled when a thumbnail is done (a delete waits for those of its hashes). */
  std::condition_variable delete_cv_;
  /** One delete at a time. */
  std::mutex delete_serial_;
  /**
   * Under mutex_: the storage grid's thumbnail jobs not started yet, the workers running them, and the (chat, hash)
   * the hook could not thumbnail (valid bytes; forgotten at the next unlock).
   */
  std::deque<GridThumbnailJob> grid_queue_;
  /** Workers running grid and chat jobs (one limit, kMaxGridThumbnailJobs, for both). */
  size_t grid_running_ = 0;
  /**
   * Under mutex_: the threads grid and chat jobs run on (kMaxGridThumbnailJobs of them), not the shared worker pool:
   * there slow work (a session's configure) held the previews back for seconds. Started by the first claim; idle,
   * they block in the pool's wait. Joined by the destructor, after its jobs are done.
   */
  std::unique_ptr<WorkerPool> preview_pool_;
  /** Under mutex_: the destructor began; no job starts and no worker is claimed. */
  bool previews_stopped_ = false;
  std::unordered_set<std::string> grid_failed_;
  /**
   * Under mutex_: the chat's preview requests not started yet (the back is the newest, taken first), the job keys
   * queued or being made, and those the platform could not make from valid bytes (not tried again this run).
   */
  std::vector<GridThumbnailJob> chat_queue_;
  std::unordered_set<std::string> chat_preview_keys_;
  std::unordered_set<std::string> chat_preview_failed_;
};

} // namespace pbr
