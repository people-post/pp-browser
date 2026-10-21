#pragma once

#include <optional>
#include <string>

namespace pbr {

/**
 * What the user is told when a message of ours did not go out, or one sent to us could not be read.
 * Kinds, not words: the GUI words them in the user's language. The transport's own text stays
 * `detail`, for the log.
 */

/** Why a send failed, as far as the transports' errors tell. */
enum class SendFailure {
  Unknown,
  /** Direct messaging (the mesh) is off on this device. */
  MeshOff,
  /** The contact's connection details are malformed. */
  BadAddress,
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
};

/**
 * The kind of a send failure from a transport's error text. The transports report text only today;
 * this is the one place that reads it. `trusted_peer`: a contact we trust (its rate limit is the
 * relay being busy, not us sending too much to strangers).
 */
SendFailure ClassifySendFailure(const std::string& error_message, bool trusted_peer);

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
};

/** Stable names (stored in a local receive-failure line's payload). */
const char* ReceiveFailureWire(ReceiveFailure reason);
std::optional<ReceiveFailure> ReceiveFailureFromWire(const std::string& wire);

struct DeliveryNotice {
  enum class Kind {
    SendFailed,
    /** The peer could not be reached directly; the message went via the relay. */
    SentViaRelay,
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
  /** The transport's or pipeline's own text, for the log. */
  std::string detail;
};

} // namespace pbr
