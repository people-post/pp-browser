#pragma once

#include "common/chat/SystemControl.h"

#include "common/Module.h"
#include "lib/messaging/IThreadMentionsStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/messaging/TranscriptAssembly.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "domain/attachments/AttachmentFetchWorkflow.h"
#include "feature/conversations/PeerDisplaySources.h"

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
  /** A video's poster frame, or a sound file's cover from its own tags (empty: none). */
  std::string poster_path;
  /** A sound file's own title and artist and its length, read on this device from its tags (empty / 0: none). */
  std::string audio_title;
  std::string audio_artist;
  int64_t audio_duration_ms = 0;
  /**
   * False for a picture that must not be decoded for display: its header is unreadable or declares more pixels
   * than AttachmentLimits::max_image_pixels. Such a card is a file row with "Open".
   */
  bool inline_image_ok = true;
  /** A picture's size in pixels the right way up (its EXIF orientation applied); 0 when unknown. */
  int image_width = 0;
  int image_height = 0;
  /**
   * A file above the direct size: stored encrypted, shown as a file row (a video: a placeholder with its size)
   * with Open until an Open has made its view for this session. `local_path` is that view, empty before.
   */
  bool on_demand = false;
  /** Percent of a download in progress (0..100), -1 when not downloading or not yet known. */
  int download_percent = -1;
};

/** What a voice message's bubble shows while it plays (all zero when it is not the one playing). */
struct VoiceBubbleState {
  bool playing = false;
  bool paused = false;
  int64_t position_ms = 0;
  int64_t duration_ms = 0;
};

/** What drawing a transcript needs besides its entries: names, control lines, attachment state. */
struct TranscriptFacts {
  std::function<std::string(const std::string& contact_id)> sender_label;
  /** Local file of a sender's cached profile photo; empty: none cached, the letter avatar shows. */
  std::function<std::string(const std::string& contact_id)> sender_icon;
  /** The id a sender's letter-avatar colour is seeded with (the person's account id when known). */
  std::function<std::string(const std::string& contact_id)> sender_stable_id;
  /** A feature's control message line, names included; none: the message's own text. */
  std::function<std::optional<std::string>(const ThreadMessage&)> control_line;
  std::function<TranscriptAttachmentView(const ThreadMessage&)> attachment;
  /** A group member's public name (card nickname, then directory nickname, then short id; never a local remark). */
  std::function<std::string(const std::string& account_id)> member_name;
  /** This account's id: a group line about it reads "you". */
  std::string local_account;
  /**
   * Voice messages: drawn as a play / pause bubble when this build can play them (false on Linux: a file row),
   * with the length clamped to `voice_max_seconds` and the progress `voice_state` gives. The GUI sets these.
   */
  bool voice_supported = false;
  int voice_max_seconds = 120;
  std::function<VoiceBubbleState(const std::string& message_id)> voice_state;
  /** Sound files (an MP3 sent as a file) play in place with the voice controls, not in the system's viewer. */
  bool audio_files_play = false;
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

  /** The chats the user has (a direct chat made only by a group's events is left out until it is theirs). */
  Roe<std::vector<Thread>> ListThreads();
  /** The user did something with this chat (opened, pinned it): it is listed from now on. */
  void KeepListed(const std::string& thread_id);
  Roe<Thread> GetActiveThread() const;
  const std::string& ActiveThreadId() const { return active_thread_id_; }

  Roe<Thread> OpenThread(const std::string& thread_id);
  void ClearActiveThread();
  Roe<Thread> CreateNewAiThread();
  /** A new AI thread, stored but not opened (the open chat stays as it is). */
  Roe<Thread> CreateAiThread();
  Roe<Thread> CreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id);
  Roe<Thread> FindOrCreateDirectThread(const std::string& contact_id, ThreadChannel channel);
  Roe<Thread> CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids);
  Roe<void> SetThreadLocalTitle(const std::string& thread_id, const std::string& local_title);
  /**
   * Names an AI thread after its first question (`first_message` is the raw text; the title is cut to
   * kAiThreadTitleMaxChars display characters, and a sensitive message keeps the default title). Applies only
   * while the thread still has kDefaultAiThreadTitle; does not notify (the caller refreshes the list as part of
   * the send).
   */
  Roe<void> NameAiThreadFromFirstMessage(const std::string& thread_id, const std::string& first_message);

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
  void SetThreadMentions(IThreadMentionsStore* mentions) { mentions_ = mentions; }

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
  /** Pixel-cap verdict and upright size per view file, so a render does not re-read the head of every picture. */
  struct InlineImageFacts {
    bool ok = false;
    int width = 0;
    int height = 0;
  };
  mutable std::unordered_map<std::string, InlineImageFacts> inline_image_ok_;
  std::string active_thread_id_;
  IThreadMentionsStore* mentions_ = nullptr;
  ThreadChangedCallback on_thread_changed_;
};

} // namespace pbr
