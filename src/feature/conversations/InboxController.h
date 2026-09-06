#pragma once

#include "common/Module.h"
#include "domain/people/ContactsStore.h"
#include "domain/ui/ChatWidgetTypes.h"
#include "domain/messaging/CallTypes.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "feature/conversations/DirectoryShadowCache.h"
#include "feature/conversations/AttachmentFetchWorkflow.h"
#include "feature/conversations/PeerDisplayResolver.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipWorkflow;

class InboxController : public Module {
public:
  InboxController(IThreadStore& store, ContactsStore& contacts, PeerDisplayResolver& labels,
                  DirectoryShadowCache* shadows = nullptr);

  void SetGroupMembership(GroupMembershipWorkflow* groups);

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

  Roe<void> CloseThread(const std::string& thread_id);
  Roe<void> ClearThreadHistory(const std::string& thread_id, bool forget_memory);
  Roe<void> ForgetThreadMemory(const std::string& thread_id);

  void MarkThreadRead(const std::string& thread_id);
  void IncrementUnread(const std::string& thread_id, int delta = 1);
  void OnInboundMessagePersisted(const std::string& thread_id,
                                 const std::optional<std::string>& preview = std::nullopt);
  int SumUnread() const;
  int SumUnreadForContact(const std::string& contact_id) const;
  Roe<void> UpdatePreview(const std::string& thread_id, const std::string& preview);

  /**
   * Build visible transcript rows (D031).
   * @param oldest_inclusive when set, load pages until this display_order is included
   *        (expanded window after scroll-up); otherwise newest page only.
   */
  std::vector<MessageDisplayRow> BuildDisplayRows(
      const std::string& thread_id, std::optional<int64_t> oldest_inclusive = std::nullopt,
      std::optional<int64_t> newest_inclusive = std::nullopt) const;
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
  std::string ResolveRowClass(const std::string& sender_contact_id) const;
  std::string BuildMessageRml(const ThreadMessage& message) const;
  std::string BuildSystemRml(const ThreadMessage& message) const;
  std::string BuildCallHistoryRml(const ThreadMessage& message, CallControlType type) const;
  std::string BuildContactCardRml(const ThreadMessage& message) const;
  std::string BuildCryptoTxRml(const ThreadMessage& message) const;
  std::string BuildAttachmentRml(const ThreadMessage& message) const;
  std::string BuildUnsupportedRml(const ThreadMessage& message) const;
  std::string BuildSharedBadgeHtml(const ThreadMessage& message) const;
  std::string FormatCallPeerLabel(const std::string& identity) const;

  IThreadStore& store_;
  ContactsStore& contacts_;
  PeerDisplayResolver& labels_;
  DirectoryShadowCache* shadows_ = nullptr;
  GroupMembershipWorkflow* groups_ = nullptr;
  std::string profile_data_dir_;
  AttachmentFetchWorkflow* attachment_downloads_ = nullptr;
  std::string active_thread_id_;
  ThreadChangedCallback on_thread_changed_;
};

} // namespace pbr
