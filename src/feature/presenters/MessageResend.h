#pragma once

#include "common/Error.h"
#include "common/thread/ThreadTypes.h"
#include "lib/delivery/DeliveryNotice.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/**
 * A tap on the red mark of a message we sent (#210 step 2, DEV/PLAN-message-resend.md; B236): a message of ours in a
 * public chat (1:1 or group) whose automatic sending has completely stopped — text, picture, file, voice message, card
 * or payment — is sent again as itself (the same message id, the same row: a recipient that has it already keeps it
 * once); in a private chat, or an "@ai++" question, the pill only says why. An edited message whose own mark is gone
 * sends its edit again instead (MessageEdit).
 */
struct SendMarkTap {
  enum class Kind {
    /** Not ours, not red, or gone. */
    Nothing,
    /** A request of it is still queued or in flight (DeliveryOutbox::SendStartedMs): nothing is sent. */
    StillTrying,
    /** Not re-sent with a tap: the pill says why (`refusal`). */
    ReasonOnly,
    /** An edit sent again could not be made (`error`); the bubble stays as it was. */
    SendFailed,
    /** An edit sent again as `new_id` (MessageEdit); the unsent one is gone (`old_removed`). */
    Resent,
    /**
     * To be sent again as itself (B236): decided only; the caller starts it (ConversationsFacade::ResendMessageAsync),
     * which may still find a picture's file gone from the server.
     */
    ResendSameMessage,
    /**
     * The server could not find the recipient (a 1:1 chat with a contact): update their card first (`contact_id`),
     * and send this message again when it is resendable. Nothing is sent by the tap itself.
     */
    UpdateTheirCard,
    /** The server does not know us (a 1:1 chat with a contact): update our own card first. Nothing is sent by the tap. */
    UpdateMyCard,
  };
  /** Why ReasonOnly. */
  enum class Refusal {
    None,
    /** A private (e2e) chat, or an "@ai++" question shared with the peer. */
    ThisKind,
  };
  Kind kind = Kind::Nothing;
  Refusal refusal = Refusal::None;
  std::optional<Error> error;
  std::string new_id;
  bool old_removed = false;
  /** UpdateTheirCard: the contact whose card is updated. */
  std::string contact_id;
  /** UpdateTheirCard / UpdateMyCard: a tap would send this message again by today's rules (else only its reason). */
  bool resendable = false;
};

/** What the words for a failed message add to its reason: how a tap on its red mark goes on. */
enum class SendAdvice {
  None,
  /** The tap offers to update their card, and the message is sent again after it. */
  UpdateTheirCardAndResend,
  /** The tap offers to update their card; the message is not sent again (the user sends it again). */
  UpdateTheirCardOnly,
  UpdateMyCard,
};
SendAdvice AdviceOf(const SendMarkTap& tap);

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
  /** Why it did not go out, as its stored line says (a 1:1 chat only); none: not known. */
  std::optional<SendFailure> failure;
  /** The 1:1 chat's peer when it is a contact (its id); none: a group, or not a contact. */
  std::optional<std::string> peer_contact_id;
  /**
   * False when the tap only re-sends (after the contact's or our own card was updated): the guidance branches
   * (UpdateTheirCard, UpdateMyCard) are skipped.
   */
  bool offer_fix = true;
};

/** The decision alone (nothing is sent here): Nothing, StillTrying, ReasonOnly, ResendSameMessage, or a card fix. */
SendMarkTap DecideSendMarkTap(const SendMarkTapFacts& facts);

} // namespace pbr
