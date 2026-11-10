#pragma once

#include "common/Error.h"
#include "common/ShareableLine.h"
#include "common/chat/IDirectMessageClient.h"
#include "common/thread/ThreadTypes.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * What the user is told when a message of ours did not go out, or one sent to us could not be read.
 * Kinds, not words: the GUI words them in the user's language. The transport's own text stays
 * `detail`, for the log.
 */

/** Why a send failed, by the transports' codes. */
enum class SendFailure {
  Unknown,
  /** Direct messaging (the mesh) is off on this device. */
  MeshOff,
  /** The contact has no usable connection details. */
  NoAddress,
  /** Waiting out a backoff before dialing the peer again. */
  BackingOff,
  /** Too many dials in flight. */
  DialBusy,
  /** The peer did not answer (offline, or the address is stale). */
  PeerNoAnswer,
  /** Reached the peer, but the chat stream did not open. */
  HandshakeFailed,
  /** The direct send was not acknowledged (the relay carries it when it can). */
  DirectUnconfirmed,
  RelayNotConfigured,
  /** The relay limited us; for a trusted peer that reads as busy (it retries). */
  RelayBusy,
  RelayRateLimited,
  /** The relay could not be reached (offline, DNS, connect, TLS, timeout). */
  RelayUnreachable,
  /** The relay answered, and did not take the message. */
  RelayRejected,
};

/** `failure` as a word for a shareable line (common/Diag.h). */
ShareWord SendFailureWord(SendFailure failure);
std::optional<SendFailure> SendFailureFromWire(const std::string& wire);

/** A direct send's failure, by its code. */
SendFailure SendFailureFromDirect(DirectSendErr code);

/**
 * A relay send's failure, by its app error category and code (foundation/error/AppError). A rate
 * limit reads `RelayRateLimited`; `SendFailureForThread` turns it into `RelayBusy` for a trusted peer.
 */
SendFailure SendFailureFromRelay(const Error& error);

/** `failure` as the notice for a thread with this peer: a trusted contact's rate limit is the relay being busy. */
SendFailure SendFailureForThread(SendFailure failure, bool trusted_peer);

/** Why an inbound message could not be read or applied. */
enum class ReceiveFailure {
  ApplyFailed,
  ThreadLookupFailed,
  SenderMismatch,
  StoreError,
  SignatureCheckFailed,
  BadSignature,
  MissingLocalIdentity,
  MissingKey,
  DecryptFailed,
  DecodeFailed,
  Unsupported,
  OpenChatFailed,
  KeyUpdateFailed,
  MembershipUpdateFailed,
  ReactionRejected,
  /** A group invite from someone this device has not added as a contact (the default receiver setting). */
  GroupInviteNeedsContact,
};

/** Stable names (stored in a local receive-failure line's payload). */
const char* ReceiveFailureWire(ReceiveFailure reason);
/** The same name as a word for a shareable line (common/Diag.h). */
ShareWord ReceiveFailureWord(ReceiveFailure reason);
std::optional<ReceiveFailure> ReceiveFailureFromWire(const std::string& wire);

struct DeliveryNotice {
  enum class Kind {
    SendFailed,
    /** A group send reached some members only. */
    GroupMembersMissed,
    ReceiveFailed,
  };
  Kind kind = Kind::SendFailed;
  SendFailure send = SendFailure::Unknown;
  ReceiveFailure receive = ReceiveFailure::DecodeFailed;
  /** ReceiveFailed: who sent it. */
  std::string peer;
  /** ReceiveFailed / ApplyFailed: the system-control family whose update failed. */
  std::string family;
  /** SendFailed / GroupMembersMissed: the chat and the message that did not go out (empty: not known). */
  std::string thread_id;
  std::string message_id;
  /** The transport's or pipeline's own text, for the log. */
  std::string detail;
};

/**
 * A local transcript line saying a message did not go out (or reached part of a group only): a stored System
 * row with no words, its `payload_json` naming `control_type` `send_failure` and the reason, which the
 * transcript words in the reader's language (like a receive failure's line). One per failed message.
 */
inline constexpr const char* kSendFailureControlType = "send_failure";
/** The reason of a row for a group send that reached some members only (else a SendFailureWord). */
inline constexpr const char* kGroupMembersMissedWire = "group_members_missed";

/** The row's id for `message_id`: a second notice for the same message finds it already there. */
std::string SendFailureRowId(const std::string& message_id, const std::string& reason_wire);
ThreadMessage MakeSendFailureRow(const std::string& thread_id, const std::string& message_id,
                                 const std::string& reason_wire, int64_t timestamp_ms);
/** The reason of a send-failure row; none for any other message. */
std::optional<std::string> SendFailureRowReason(const ThreadMessage& message);

} // namespace pbr
