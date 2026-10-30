#pragma once

#include "common/chat/SystemControl.h"
#include "lib/messaging/GroupTypes.h"

#include "common/Module.h"
#include "lib/people/ContactsStore.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "domain/peers/PeerDisplaySources.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
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
};

/** One @mention to highlight in a message text: UTF-8 byte range [start, end); `me` when it is this account. */
struct TranscriptMention {
  uint32_t start = 0;
  uint32_t end = 0;
  bool me = false;
};

/** What drawing a transcript needs besides its entries: names, control lines, attachment state. */
struct TranscriptFacts {
  std::function<std::string(const std::string& contact_id)> sender_label;
  /** A feature's control message line, names included; none: the message's own text. */
  std::function<std::optional<std::string>(const ThreadMessage&)> control_line;
  std::function<TranscriptAttachmentView(const ThreadMessage&)> attachment;
  /** The message's @mentions that count by the roster now (members; `all` only from the owner). None: no highlight. */
  std::function<std::vector<TranscriptMention>(const ThreadMessage&)> mentions;
};

/** Stored title of a new AI thread until its first question names it (shown localized by the GUI). */
inline constexpr const char* kDefaultAiThreadTitle = "New chat";

class InboxController : public Module {
public:
  InboxController(IThreadStore& store, ContactsStore& contacts, PeerDisplayResolver& labels,
                  DirectoryShadowCache* shadows = nullptr);

  void SetGroupMembership(GroupMembershipWorkflow* groups);
  /** Who this account is and a group's current roster: what @mentions are checked against (when shown / arriving). */
  void SetMentionContext(std::function<std::string()> self_account_id,
                         std::function<std::vector<GroupRosterMember>(const std::string& group_id)> roster);
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
  /**
   * `message`: the stored message, so an @mention of this account can be told (by the roster as it is now).
   * `live`: false for messages that only fill in history (sync): they never set the "mentioned you" marker.
   */
  void OnInboundMessagePersisted(const std::string& thread_id, const std::optional<std::string>& preview = std::nullopt,
                                 const ThreadMessage* message = nullptr, bool live = true);
  /** True when `message` (stored in a group thread) @-mentions this account by the roster as it is now. */
  bool MessageMentionsMe(const std::string& group_id, const ThreadMessage& message) const;
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
  std::function<std::string()> mention_self_account_id_;
  std::function<std::vector<GroupRosterMember>(const std::string& group_id)> mention_roster_;
  std::string profile_data_dir_;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::string active_thread_id_;
  ThreadChangedCallback on_thread_changed_;
};

} // namespace pbr
