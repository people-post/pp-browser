#pragma once

#include "common/Error.h"
#include "common/thread/ThreadTypes.h"
#include "lib/delivery/TextSpans.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * A tap on the red mark of a message we sent (#210 step 2, DEV/PLAN-message-resend.md): an own text in a public chat
 * (1:1 or group) whose automatic sending has completely stopped is sent again as a new message, and the unsent row
 * goes; anything else only says why. Nothing in delivery changes: the new message is an ordinary one.
 */
struct SendMarkTap {
  enum class Kind {
    /** Not ours, not red, or gone (a second tap after the row was taken away). */
    Nothing,
    /** A request of it is still queued or in flight (DeliveryOutbox::SendStartedMs): nothing is sent. */
    StillTrying,
    /** Not re-sent with a tap: the pill says why (`refusal`). */
    ReasonOnly,
    /** The new message could not be made (`error`); the old bubble stays as it was. */
    SendFailed,
    /** Sent as `new_id`; the old row is gone (`old_removed`), or kept because it was delivered meanwhile. */
    Resent,
    /** Sent again earlier, but its old row could not be taken away then, nor now: nothing is sent a third time. */
    RemovalFailed,
  };
  /** Why ReasonOnly. */
  enum class Refusal {
    None,
    /** A private (e2e) chat, or an "@ai++" question shared with the peer. */
    ThisKind,
    /** A picture, file, voice message or card (it would be uploaded again). */
    NotText,
  };
  Kind kind = Kind::Nothing;
  Refusal refusal = Refusal::None;
  std::optional<Error> error;
  std::string new_id;
  bool old_removed = false;
  /** The store refused the removal (an error, not "delivered meanwhile"): the old row is red still. */
  bool removal_error = false;
};

/** What a tap is decided from. */
struct SendMarkTapFacts {
  /** The tapped row, read now; none when it is gone. */
  std::optional<ThreadMessage> row;
  std::optional<Thread> thread;
  /** When its send still under way started (DeliveryOutbox::SendStartedMs); none: nothing of it is alive. */
  std::optional<int64_t> started_ms;
  int64_t now_ms = 0;
  /** A group message that had nobody to go to (GroupRowHadRecipients). */
  bool no_recipients = false;
  /** It was sent again already and only its removal failed: a tap retries the removal, never the send. */
  bool already_resent = false;
};

/** The decision alone: Nothing, StillTrying, ReasonOnly, or — kind Resent, nothing done yet — that it is re-sent. */
SendMarkTap DecideSendMarkTap(const SendMarkTapFacts& facts);

/** What re-sending reads and does: the facade's, or a test's. */
struct ResendPorts {
  /** Group mentions still on the roster (ChatPresenter::SpansOnRoster). */
  std::function<std::vector<TextSpan>(std::vector<TextSpan> spans)> spans_on_roster;
  /** The composer's sends, returning the row they made: 1:1 SendUserMessage, group SendGroupText. */
  std::function<Roe<ThreadMessage>(const std::string& text)> send_direct;
  std::function<Roe<ThreadMessage>(const std::string& text, std::vector<TextSpan> spans)> send_group;
  /** ConversationsFacade::RemoveUnsentMessage. */
  std::function<Roe<bool>(const std::string& message_id)> remove_unsent;
};

/**
 * DecideSendMarkTap, then for a re-send: the new message first (the same text — a quote reply's quote is in it — and,
 * in a group, its mentions still on the roster), and only once it exists the old row goes. A failed send leaves
 * everything as it was. Runs on the UI thread from tap to removal, so a second tap finds the row gone.
 */
SendMarkTap ResendAfterTap(const SendMarkTapFacts& facts, const ResendPorts& ports);

} // namespace pbr
