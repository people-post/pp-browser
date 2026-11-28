#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "lib/delivery/SendRelayOptions.h"
#include "lib/messaging/OutgoingAttachment.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Forwarding chat messages, text and contact cards (DEV/PLAN-message-forward-text.md). Nothing here changes a wire or
 * disk format: a forwarded message is an ordinary new message of ours (the current text, no "forwarded" mark, no
 * mentions), a forwarded card is the stored card sent again as it is.
 */

/** At most this many messages go in one forward, to at most this many chats. */
inline constexpr size_t kMaxForwardMessages = 30;
inline constexpr size_t kMaxForwardTargets = 3;
/**
 * What the attachments of one forward may add up to (DEV/PLAN-message-forward-files-v3.md, the owner's final limits):
 * at most this many ticked pictures / videos / files / voice messages and this many bytes, counted over the ticked items
 * once, not once per chat. Text and cards never count, nor does an item this device holds no bytes of.
 */
inline constexpr size_t kMaxForwardAttachments = 3;
inline constexpr uint64_t kMaxForwardUploadBytes = 25'000'000;
/** What the encrypted stored copy may weigh over its plaintext (the check on the stored size, before decrypting). */
inline constexpr uint64_t kForwardStoredOverheadSlack = 1'000'000;
/** The run waits this long for the item before an attachment to leave the queue; then the rest of that chat fails. */
inline constexpr int64_t kForwardSettleGiveUpMs = 60000;
/**
 * A forward's upload that was let go of (Close, a tap, the background) still counts as "an attachment is being sent" until
 * it answers, so two uploads never run together; if the messaging layer never answers, after this long it no longer does.
 * Ten minutes: a 25 MB forward on a 1 Mbps uplink takes about 3.5 minutes, so this is far past any upload that is alive.
 */
inline constexpr int64_t kForwardOrphanBusyMs = 600000;
/** An upload the send pace refused is tried again this often; a cancel that got no answer offers "close" after it. */
inline constexpr int64_t kForwardRateLimitRetryMs = 15000;
inline constexpr int64_t kForwardCloseOfferMs = 15000;
/** How often the state of the message just before an attachment (or after it) is looked at while it is still queued. */
inline constexpr int64_t kForwardSettlePollMs = 500;
/** A forward the phone stopped with an item under way tells its notice when that item answers, or after this long. */
inline constexpr int64_t kForwardNoticeWaitMs = 60000;

/**
 * Whether `message` can be forwarded: text (also quote replies, edited ones, and an AI chat's questions and answers),
 * contact cards whose stored card can be sent again as it is, and attachments (picture, video, file, voice) whose
 * fields can be read (received, or ours and sent: a queued or failed one of ours is not). Whether this device still has
 * the attachment's bytes is not decided here (the forward says so when it reaches it). Not: on-device app-action rows, system lines, annotations, crypto lines, tombstones, unsupported rows, an AI question
 * that carries a picture, and our own messages that are still queued or did not go out.
 */
bool CanForwardMessage(const ThreadMessage& message);

/**
 * The strip of the "Forward to" list: one text message shows its first two lines, one contact card its name, several
 * messages `count_text(count)` ("3 messages"). Empty when nothing is chosen.
 */
std::string ForwardPreviewText(const std::vector<ThreadMessage>& messages,
                               const std::function<std::string(size_t count)>& count_text);

/** What a chat can take a forward as. */
enum class ForwardTargetKind { Unavailable, Direct, Group };

/** What decides whether a chat can take a forward; the presenter gathers them from the messaging layer. */
struct ForwardTargetFacts {
  /** A one-to-one chat: its peer is a saved contact, and that contact is blocked. */
  bool saved_contact = false;
  bool blocked = false;
  /** A group chat: we are still a member (not removed or left), and its owner has not ended it. */
  bool in_group = false;
  bool group_dissolved = false;
  /** This account's id, and the PP support account's (empty when there is none): a chat with either is no target. */
  std::string my_account;
  std::string support_account;
};

/**
 * What `thread` is as somewhere to forward to: a saved, unblocked contact's chat, or a group we may post in. Not: an AI
 * conversation, a chat with ourselves, the PP support chat, a blocked contact, a chat with someone who is not a saved
 * contact, a group we left or that ended.
 */
ForwardTargetKind ClassifyForwardTarget(const Thread& thread, const ForwardTargetFacts& facts);

/** A chat or contact as the messaging layer knows it, before the presenter decides whether it is a target. */
struct ForwardCandidate {
  /** The chat; for a contact with no chat yet a Direct thread with an empty id and the contact's identity. */
  Thread thread;
  std::string contact_id;
  std::string title;
  std::string subtitle;
  ForwardTargetFacts facts;
};

/** One row of the "Forward to" list. A contact with no chat yet has no `thread_id`: the chat is made when it is chosen. */
struct ForwardTarget {
  std::string thread_id;
  std::string contact_id;
  std::string title;
  std::string subtitle;
  bool group = false;
  int64_t updated_at = 0;

  /** What the list and the picker know it by: the chat's id, else "contact:<id>". */
  std::string Key() const { return thread_id.empty() ? "contact:" + contact_id : thread_id; }
};

/** The most recently active chats that lead the list. */
inline constexpr size_t kForwardRecentChats = 8;

/**
 * The list's order, each chat or person once: the most recent chats (kForwardRecentChats, newest first), then the
 * other people (a contact whose chat is among the recent ones is not listed twice), then the other groups; each part
 * by name.
 */
std::vector<ForwardTarget> OrderForwardTargets(std::vector<ForwardTarget> targets);

/** What a forward item is, for the preview and the limits. */
enum class ForwardItemKind { Text, Card, Picture, Video, Voice, File };

/** One chosen message as the "Forward to" page and the limits see it (nothing of it is loaded or decrypted). */
struct ForwardItemFacts {
  std::string message_id;
  ForwardItemKind kind = ForwardItemKind::Text;
  /** Attachments only: the declared size and name, and whether this device holds the bytes (a stat, no decrypting). */
  uint64_t byte_length = 0;
  std::string mime;
  std::string filename;
  std::vector<uint8_t> content_hash;
  bool has_local_bytes = true;
};

/** What `message` is as a forward item (an attachment's fields read; none when it is not a forwardable attachment). */
ForwardItemKind ForwardKindOf(const ThreadMessage& message);

/** Why a forward is not started (the "Forward to" page says it above the preview; nothing has been sent). */
enum class ForwardRefusal {
  None,
  /** Another attachment of ours is still being sent. */
  AttachmentBusy,
  /** More than kMaxForwardAttachments ticked attachments. */
  TooManyUploads,
  /** More than kMaxForwardUploadBytes in all (split it over several forwards). */
  TooLarge,
  /** One attachment alone is above kMaxForwardUploadBytes: it cannot be forwarded at all. */
  SingleTooLarge,
  /** A call is on: the progress page would hide its controls (checked by the presenter, not by CheckForwardLimits). */
  CallActive,
};

/** The limits of a forward worked out before anything starts, from the declared sizes and what is held locally. */
struct ForwardCheck {
  ForwardRefusal refusal = ForwardRefusal::None;
  /** Chosen attachments, of which this device does not hold the bytes (they will not be forwarded, nor counted). */
  size_t attachments = 0;
  size_t without_local_bytes = 0;
  /** Ticked attachments this device holds the bytes of (each once, whatever the number of chats), and their declared sizes. */
  size_t uploads = 0;
  uint64_t upload_bytes = 0;
  /** The largest declared size among the attachments that would go (for the "this file is too large" line). */
  uint64_t largest_bytes = 0;
  /** Per chosen message, in the chat's order. */
  std::vector<ForwardItemFacts> items;
};

/**
 * The limits for `items`, however many chats they go to. Pure: the caller read `has_local_bytes` without decrypting. An
 * attachment without local bytes is in no count; text and cards are never counted. `attachment_busy` is checked first.
 */
ForwardCheck CheckForwardLimits(std::vector<ForwardItemFacts> items, bool attachment_busy);

/**
 * What a forward needs from the messaging layer. The presenter fills it from the conversations facade; the send ports
 * are the facade's `SendUserMessage` / `SendGroupMessage` / `SendAttachmentAsync`.
 */
struct ForwardPorts {
  /** The chat open now (the forward's source). */
  std::function<std::string()> active_thread;
  /** The message as stored now; none when it is gone (retracted, deleted). */
  std::function<std::optional<ThreadMessage>(const std::string& thread_id, const std::string& message_id)> get_message;
  /** What `thread_id` is as a target now: a contact's chat we may write to, a group we may post in, or neither. */
  std::function<ForwardTargetKind(const std::string& thread_id)> target_of;
  /** This account's id now (empty when there is none): a forward stopped part-way is told about to this account only. */
  std::function<std::string()> account_id;
  /** Every chat and every saved contact, undecided: the list of "Forward to" is made from them. */
  std::function<std::vector<ForwardCandidate>()> candidates;
  /** The one-to-one chat with a contact, made when there is none yet; empty when it cannot be made. */
  std::function<std::string(const std::string& contact_id)> direct_thread_of;
  std::function<Roe<ThreadMessage>(const std::string& thread_id, const std::string& text, SendRelayOptions options)>
      send_user;
  std::function<Roe<ThreadMessage>(const std::string& thread_id, const std::string& text, SendRelayOptions options)>
      send_group;
  /**
   * The encrypted copies of attachments the messaging layer holds for a retry in `thread_id` (ids; each belongs to a
   * failed bubble with Retry), or none when that cannot be told (locked, no key, the folder unreadable). A send that
   * fails after leaving one there is a failed send, never to be sent again by the run; one that leaves none was refused
   * before anything was stored (the device's pace) and may be tried again. When the answer is "cannot tell", before or
   * after the send, or the port is unset, a rate-limited attachment is counted failed and never retried.
   */
  std::function<std::optional<std::vector<std::string>>(const std::string& thread_id)> staged_attachments;
  /** The messaging layer can take a send now (not torn down, not locked). Unset: always. */
  std::function<bool()> messaging_ready;
  /** A saved contact we may still write to (not blocked meanwhile); asked before a chat is made for them. Unset: yes. */
  std::function<bool(const std::string& contact_id)> contact_can_receive;

  // --- Attachments (DEV/PLAN-message-forward-files-v3.md). A forward with none of them calls none of these. ---------
  /**
   * The size on disk of the stored copy of an attachment's bytes, 0 when this device has none. A stat of the content
   * store: nothing is read or decrypted, so asking costs nothing and is allowed while only previewing.
   */
  std::function<uint64_t(const std::vector<uint8_t>& content_hash)> local_bytes;
  /** The whole plaintext of a stored attachment (decrypts it). Called only when that attachment's turn comes. */
  std::function<Roe<ByteVector>(const std::vector<uint8_t>& content_hash)> load_plaintext;
  /**
   * Starts sending `attachment` (as it is, never shrunk) to `thread_id`. `on_done` is called once, later, on the UI
   * thread (the facade's callback comes from a worker; the presenter's port hands it over), with the stored message or
   * the error; the attachment pace's refusal is `IsAttachmentRateLimited` and comes before anything is stored.
   * `progress` may be called from any thread.
   */
  std::function<void(const std::string& thread_id, OutgoingAttachment attachment,
                     std::function<void(Roe<ThreadMessage>)> on_done,
                     std::function<bool(uint64_t done, uint64_t total)> progress)>
      send_attachment;
  /** Monotonic milliseconds, and "run this on the UI thread after so many milliseconds" (retries, polls, the 15 s offers). */
  std::function<int64_t()> now_ms;
  std::function<void(int64_t delay_ms, std::function<void()> task)> after;
};

/** Why a forward stopped before the end. */
enum class ForwardStop {
  None,
  /** The user's Cancel: what is under way is waited for. */
  Cancelled,
  /** A notification tap closed the page: handled like a Cancel, said differently. */
  Tapped,
  /** The phone really went to the background, the vault locked, an account signed out, messaging went away: at once. */
  Interrupted,
  /** The chat the messages came from was deleted elsewhere. */
  SourceGone,
  /** A Cancel the messaging layer never answered, closed by the user after kForwardCloseOfferMs. */
  Abandoned,
};

/** What a forward did, for the one line the user gets and the metric. Counts of messages unless said. */
struct ForwardResult {
  /** Sends that were queued, over all chats. */
  size_t forwarded = 0;
  /** Messages dropped by the re-check just before sending (retracted or deleted meanwhile). */
  size_t skipped_messages = 0;
  /** Chats dropped by the re-check (no longer a contact we may write to / a group we may post in). */
  size_t skipped_targets = 0;
  /** Messages the messaging layer refused in at least one chat (a red mark there, or nothing stored). */
  size_t send_errors = 0;
  /** Not one send was queued: "forward failed". */
  bool nothing_sent = false;
  /** The batch was stopped (sign-out, messaging torn down, lock) before it finished. */
  bool interrupted = false;
  /** Messages x chats, the b of "forwarded a of b" (`forwarded` is the a). */
  size_t total_items = 0;
  /** Attachment items (attachments x chats) and how many of them did not go (no local bytes, refused, too big...). */
  size_t attachments = 0;
  size_t attachments_failed = 0;
  /** How it ended early, if it did (`interrupted` is also set for Interrupted and SourceGone). */
  ForwardStop stop = ForwardStop::None;
  /** The forward was not started: nothing was sent, the reason is the limit that was broken. */
  ForwardRefusal refusal = ForwardRefusal::None;
};

/** What the progress page shows now. */
struct ForwardProgress {
  /** Items done / of all (text and cards count). */
  size_t done = 0;
  size_t total = 0;
  /** An attachment is being sent now; its upload so far (0 / 0 until the first report), and since when (ms, monotonic). */
  bool uploading = false;
  uint64_t bytes_done = 0;
  uint64_t bytes_total = 0;
  int64_t uploading_since_ms = 0;
  /** The send pace refused the item: it goes again by itself ("one moment"). */
  bool waiting_for_pace = false;
  /** Cancel was pressed and the item under way is being waited for. */
  bool finishing = false;
  /** ...and it got no answer for kForwardCloseOfferMs: "Close" is offered. */
  bool can_close = false;
};

/** One forward being sent: the chosen messages to the chosen chats, one chat completely before the next. */
class ForwardRun {
public:
  /**
   * `targets`: a chat by its id, or a person with no chat yet by contact id: that chat is made when the run reaches
   * them (after the re-check, right before the sends), never before.
   */
  ForwardRun(ForwardPorts ports, std::string source_thread, std::vector<std::string> message_ids,
             std::vector<ForwardTarget> targets, std::set<std::string> uploadable);

  /** Waiting: nothing more can be done until the messaging layer answers or a timer fires (the wake is then called). */
  enum class State { Running, Waiting, Done, Aborted };

  /**
   * Called (on the UI thread) when the run can go on: an attachment's answer arrived, a timer fired. Set once before
   * the first Step; the run never calls it while it is inside Step.
   */
  void SetWake(std::function<void()> wake);

  /**
   * Makes up to `budget` send calls. `still_valid` is asked before every call (and before the first re-check): when
   * it says false the run stops, and no port is called again.
   */
  State Step(size_t budget, const std::function<bool()>& still_valid);

  /** Stops starting new items. A Cancel or a Tap waits for the one under way; any other stop does not. */
  void Stop(ForwardStop kind);
  /** An attachment answer that arrived: counted into the result (also after the run was stopped; starts nothing). */
  void Collect();
  /** An attachment is being sent: its answer is not in yet. */
  bool InFlight() const;
  ForwardProgress Progress() const;

  const ForwardResult& Result() const { return result_; }
  const std::string& SourceThread() const { return source_thread_; }
  size_t MessageCount() const { return message_count_; }
  size_t TargetCount() const { return targets_.size(); }

private:
  struct Completion;

  bool Prepare();
  bool BeginTarget();
  /** The items after this one in the open chat are lost (the chat can no longer be written to). */
  void FailRestOfTarget(bool count_as_skipped = true);
  void FailMessage(size_t message_at);
  void NextItem();
  bool Settled(const std::string& thread_id, const std::string& message_id);
  void WakeAfter(int64_t delay_ms);
  /** Starts the open item's upload; true when it is under way (the run now waits for its answer). */
  bool StartAttachment();
  /** What a RateLimited answer was: the device's pace (nothing staged), a server refusal after staging, or not known. */
  enum class RateLimitKind { Pace, Staged, Unknown };
  RateLimitKind ClassifyRateLimit() const;
  void FinishRun();

  ForwardPorts ports_;
  std::string source_thread_;
  std::vector<std::string> message_ids_;
  std::vector<ForwardTarget> targets_;
  /** The chat the open target sends to (its own id, or the one made for its person). */
  std::string target_thread_;
  size_t message_count_ = 0;
  bool prepared_ = false;
  /** The messages that survived the re-check, in the chat's order. */
  std::vector<ThreadMessage> messages_;
  /** Per kept message: some chat refused it (so `send_errors` counts messages, not sends). */
  std::vector<bool> send_failed_;
  /** Per kept message: the attachment it carries (kind Attachment only). */
  std::vector<std::optional<ChatAttachmentFields>> fields_;
  bool has_attachments_ = false;
  size_t target_at_ = 0;
  size_t message_at_ = 0;
  bool target_open_ = false;
  ForwardTargetKind target_kind_ = ForwardTargetKind::Unavailable;
  ForwardResult result_;

  // Attachments only.
  std::function<void()> wake_;
  std::shared_ptr<Completion> completion_;
  bool in_flight_ = false;
  /** The item sent last in the open chat, and whether the next one must see it settled first. */
  std::string last_sent_id_;
  bool last_was_attachment_ = false;
  /** Real bytes handed over so far (the limit is on these, not on the declared sizes). */
  uint64_t uploaded_bytes_ = 0;
  size_t in_flight_bytes_ = 0;
  ForwardStop stop_ = ForwardStop::None;
  bool stopped_ = false;
  int64_t retry_at_ms_ = 0;
  int64_t finishing_since_ms_ = 0;
  int64_t uploading_since_ms_ = 0;
  bool rate_limited_ = false;
  /** The copies staged in the open chat before the upload under way began (an answer with an error is told by what is new). */
  std::optional<std::vector<std::string>> staged_before_;
  /** The attachments (message ids) that had bytes on this device when the forward was checked: only these are uploaded. */
  std::set<std::string> uploadable_;
  /** Since when the run has been waiting for `last_sent_id_` to leave the queue (0: not waiting). */
  std::optional<int64_t> settle_wait_since_ms_;
};

/** The messages chosen to forward: in the order of choosing, at most kMaxForwardMessages, from one chat. */
class ForwardSelection {
public:
  enum class Toggle { Selected, Deselected, LimitReached };

  bool Active() const { return active_; }
  const std::string& Thread() const { return thread_id_; }
  const std::vector<std::string>& Ids() const { return ids_; }
  size_t Count() const { return ids_.size(); }
  bool Has(const std::string& message_id) const;

  /** Starts choosing in `thread_id` with `message_id` ticked. */
  void Begin(const std::string& thread_id, const std::string& message_id);
  Toggle Flip(const std::string& message_id);
  void Clear();

private:
  bool active_ = false;
  std::string thread_id_;
  std::vector<std::string> ids_;
};

} // namespace pbr
