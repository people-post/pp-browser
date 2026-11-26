#pragma once

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"
#include "lib/delivery/SendRelayOptions.h"

#include <cstddef>
#include <functional>
#include <optional>
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
inline constexpr size_t kMaxForwardTargets = 9;

/**
 * Whether `message` can be forwarded: text (also quote replies, edited ones, and an AI chat's questions and answers)
 * and contact cards whose stored card can be sent again as it is. Not: attachments (the plan's second part),
 * on-device app-action rows, system lines, annotations, crypto lines, tombstones, unsupported rows, an AI question
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

/**
 * What a forward needs from the messaging layer. The presenter fills it from the conversations facade; the send ports
 * are the facade's `SendUserMessage` / `SendGroupMessage`.
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
};

/** What a forward did, for the one line the user gets and the metric. Counts of messages unless said. */
struct ForwardResult {
  /** Sends that were queued, over all chats. */
  size_t forwarded = 0;
  /** Messages dropped by the re-check just before sending (retracted or deleted meanwhile). */
  size_t skipped_messages = 0;
  /** Chats dropped by the re-check (no longer a contact we may write to / a group we may post in). */
  size_t skipped_targets = 0;
  /** Sends the messaging layer refused (a red mark in that chat, or nothing stored). */
  size_t send_errors = 0;
  /** Not one send was queued: "forward failed". */
  bool nothing_sent = false;
  /** The batch was stopped (sign-out, messaging torn down, lock) before it finished. */
  bool interrupted = false;
};

/** One forward being sent: the chosen messages to the chosen chats, one chat completely before the next. */
class ForwardRun {
public:
  ForwardRun(ForwardPorts ports, std::string source_thread, std::vector<std::string> message_ids,
             std::vector<std::string> targets);

  enum class State { Running, Done, Aborted };

  /**
   * Makes up to `budget` send calls. `still_valid` is asked before every call (and before the first re-check): when
   * it says false the run stops, and no port is called again.
   */
  State Step(size_t budget, const std::function<bool()>& still_valid);

  const ForwardResult& Result() const { return result_; }
  size_t MessageCount() const { return message_count_; }
  size_t TargetCount() const { return targets_.size(); }

private:
  bool Prepare();
  bool BeginTarget();

  ForwardPorts ports_;
  std::string source_thread_;
  std::vector<std::string> message_ids_;
  std::vector<std::string> targets_;
  size_t message_count_ = 0;
  bool prepared_ = false;
  /** The messages that survived the re-check, in the chat's order. */
  std::vector<ThreadMessage> messages_;
  size_t target_at_ = 0;
  size_t message_at_ = 0;
  bool target_open_ = false;
  ForwardTargetKind target_kind_ = ForwardTargetKind::Unavailable;
  ForwardResult result_;
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
