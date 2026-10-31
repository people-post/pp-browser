#pragma once

#include "common/chat/SystemControl.h"

#include "common/Module.h"
#include "lib/messaging/ThreadMentionsStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/peers/PeerDisplaySources.h"

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipWorkflow;

/** What a message's attachment card shows: its download state and the files on disk. */
struct TranscriptAttachmentView {
  AttachmentFetchWorkflow::DownloadState state = AttachmentFetchWorkflow::DownloadState::Pending;
  /** The viewable file (empty: none on disk, or a large private video kept off disk until opened). */
  std::string local_path;
  /** A video's poster frame (empty: none). */
  std::string poster_path;
  /**
   * False for a picture that must not be decoded for display: its header is unreadable or declares more pixels
   * than AttachmentLimits::max_image_pixels. Such a card is a file row with "Open".
   */
  bool inline_image_ok = true;
  /** Percent of a download in progress (0..100), -1 when not downloading or not yet known. */
  int download_percent = -1;
};

/** What drawing a transcript needs besides its entries: names, control lines, attachment state. */
struct TranscriptFacts {
  std::function<std::string(const std::string& contact_id)> sender_label;
  /** A feature's control message line, names included; none: the message's own text. */
  std::function<std::optional<std::string>(const ThreadMessage&)> control_line;
  std::function<TranscriptAttachmentView(const ThreadMessage&)> attachment;
};

/** Stored title of a new AI thread until its first question names it (shown localized by the GUI). */
inline constexpr const char* kDefaultAiThreadTitle = "New chat";

class InboxController : public Module {
public:
  InboxController(IThreadStore& store, ContactsStore& contacts, PeerDisplayResolver& labels,
                  DirectoryShadowCache* shadows = nullptr);

  void SetGroupMembership(GroupMembershipWorkflow* groups);
  /** The system-control families features bind: how their messages show (hidden, worded). Not owned. */
  void SetSystemControls(const SystemControlRegistry* controls) { controls_ = controls; }

  Roe<std::vector<Thread>> ListThreads();
  Roe<Thread> GetActiveThread() const;
  const std::string& ActiveThreadId() const { return active_thread_id_; }

  Roe<Thread> OpenThread(const std::string& thread_id);
  void ClearActiveThread();
  Roe<Thread> CreateNewAiThread();
  Roe<Thread> CreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  Roe<Thread> CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids);
  Roe<void> SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title);
  /**
   * Names an AI thread after its first question. Applies only while the thread still has
   * kDefaultAiThreadTitle; does not notify (the caller refreshes the list as part of the send).
   */
  Roe<void> NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& title);

  Roe<void> CloseThread(const std::string& thread_id);
  Roe<void> ClearThreadHistory(const std::string& thread_id, bool forget_memory);
  /**
   * "Delete for me": the message's content and its attachment / image bytes are deleted from this device;
   * an id-only tombstone stays so sync cannot bring it back, and the bubble shows "This message was deleted".
   * Nothing is sent.
   */
  Roe<void> DeleteMessageForMe(const std::string& thread_id, const std::string& message_id);
  /**
   * After a message's row became a tombstone (here, or by its sender's retract): delete its bytes, forget a
   * summary that folded it in, and stop the list preview from showing its text.
   */
  void AfterMessageDeleted(const std::string& thread_id, const ThreadMessage& original);
  Roe<void> ForgetThreadMemory(const std::string& thread_id);

  void MarkThreadRead(const std::string& thread_id);
  /** `even_if_active`: count it although it is the active thread (the user left its page, e.g. back to the list). */
  void IncrementUnread(const std::string& thread_id, int delta = 1, bool even_if_active = false);
  void OnInboundMessagePersisted(const std::string& thread_id,
                                 const std::optional<std::string>& preview = std::nullopt);
  int SumUnread() const;
  int SumUnreadForContact(const std::string& contact_id) const;
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview);

  /**
   * The thread's transcript entries (D031), drawn by the GUI with Facts().
   * @param oldest_inclusive when set, load pages until this display_order is included
   *        (expanded window after scroll-up); otherwise newest page only.
   */
  std::vector<TranscriptRow> BuildTranscript(const std::string& thread_id,
                                               std::optional<int64_t> oldest_inclusive = std::nullopt,
                                               std::optional<int64_t> newest_inclusive = std::nullopt) const;
  /** Bound to this controller; valid while it lives. */
  TranscriptFacts Facts() const;
  /** True when local transcript has rows older than the given display_order. */
  bool HasLocalMessagesBefore(const std::string& thread_id, int64_t before_display_order) const;

  PeerDisplayLabel ResolveThreadLabel(const Thread& thread) const;
  PeerDisplayResolver& Labels() { return labels_; }
  const PeerDisplayResolver& Labels() const { return labels_; }
  DirectoryShadowCache* Shadows() { return shadows_; }

  using ThreadChangedCallback = std::function<void()>;
  void SetOnThreadChanged(ThreadChangedCallback callback);
  void NotifyThreadChanged();

  void SetProfileDataDir(std::string profile_dir);
  void SetAttachmentDownloads(AttachmentFetchWorkflow* downloads);
  /** The unread-mention marker (G015): a thread's rows go when it is opened or deleted, a message's when it is. */
  void SetThreadMentions(ThreadMentionsStore* mentions) { mentions_ = mentions; }

private:
  std::string ResolveSenderLabel(const std::string& sender_contact_id) const;
  std::optional<std::string> ControlLine(const ThreadMessage& message) const;
  TranscriptAttachmentView AttachmentView(const ThreadMessage& message) const;

  IThreadStore& store_;
  ContactsStore& contacts_;
  PeerDisplayResolver& labels_;
  const SystemControlRegistry* controls_ = nullptr;
  DirectoryShadowCache* shadows_ = nullptr;
  GroupMembershipWorkflow* groups_ = nullptr;
  std::string profile_data_dir_;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  /** Pixel-cap verdict per view file, so a render does not re-read the head of every picture. */
  mutable std::unordered_map<std::string, bool> inline_image_ok_;
  std::string active_thread_id_;
  ThreadMentionsStore* mentions_ = nullptr;
  ThreadChangedCallback on_thread_changed_;
};

} // namespace pbr
