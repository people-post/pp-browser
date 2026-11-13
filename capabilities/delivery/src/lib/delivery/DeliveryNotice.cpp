#include "lib/delivery/DeliveryNotice.h"

#include "common/ValueJson.h"
#include "foundation/error/AppError.h"

#include <array>

namespace pbr {
namespace {

// A plain aggregate, not std::pair: pair's converting constructor would build the ShareWord at run time.
struct ReceiveWire {
  ReceiveFailure value;
  ShareWord wire;
};

constexpr std::array<ReceiveWire, 16> kReceiveWire = {{
    {ReceiveFailure::ApplyFailed, "apply_failed"},
    {ReceiveFailure::ThreadLookupFailed, "thread_lookup_failed"},
    {ReceiveFailure::SenderMismatch, "sender_mismatch"},
    {ReceiveFailure::StoreError, "store_error"},
    {ReceiveFailure::SignatureCheckFailed, "signature_check_failed"},
    {ReceiveFailure::BadSignature, "bad_signature"},
    {ReceiveFailure::MissingLocalIdentity, "missing_local_identity"},
    {ReceiveFailure::MissingKey, "missing_key"},
    {ReceiveFailure::DecryptFailed, "decrypt_failed"},
    {ReceiveFailure::DecodeFailed, "decode_failed"},
    {ReceiveFailure::Unsupported, "unsupported"},
    {ReceiveFailure::OpenChatFailed, "open_chat_failed"},
    {ReceiveFailure::KeyUpdateFailed, "key_update_failed"},
    {ReceiveFailure::MembershipUpdateFailed, "membership_update_failed"},
    {ReceiveFailure::ReactionRejected, "reaction_rejected"},
    {ReceiveFailure::GroupInviteNeedsContact, "group_invite_needs_contact"},
}};

} // namespace

SendFailure SendFailureFromDirect(const DirectSendErr code) {
  switch (code) {
    case DirectSendErr::NotStarted:
      return SendFailure::MeshOff;
    case DirectSendErr::NoAddress:
      return SendFailure::NoAddress;
    case DirectSendErr::DialInBackoff:
      return SendFailure::BackingOff;
    case DirectSendErr::DialBusy:
      return SendFailure::DialBusy;
    case DirectSendErr::NoAnswer:
    case DirectSendErr::TransportFailed:
      return SendFailure::PeerNoAnswer;
    case DirectSendErr::Refused:
    case DirectSendErr::ChannelFailed:
      return SendFailure::HandshakeFailed;
    case DirectSendErr::NotConfirmed:
      return SendFailure::DirectUnconfirmed;
    case DirectSendErr::Ok:
    case DirectSendErr::Generic:
      break;
  }
  return SendFailure::Unknown;
}

SendFailure SendFailureFromRelay(const Error& error) {
  switch (AppError::CategoryOf(error)) {
    case ErrorCategory::Auth:
      switch (static_cast<Err::Auth>(error.code)) {
        case Err::Auth::RateLimited:
        case Err::Auth::StrangerQuota:
          return SendFailure::RelayRateLimited;
        default:
          return SendFailure::RelayRejected;
      }
    case ErrorCategory::Config:
      return SendFailure::RelayNotConfigured;
    case ErrorCategory::Network:
      switch (static_cast<Err::Network>(error.code)) {
        case Err::Network::HttpError:
        case Err::Network::HttpRejected:
          return SendFailure::RelayRejected;
        case Err::Network::RecipientNotRegistered:
          return SendFailure::RecipientNotOnRelay;
        case Err::Network::PaymentRequired:
          return SendFailure::PaymentRequired;
        default:
          return SendFailure::RelayUnreachable;
      }
    default:
      return SendFailure::Unknown;
  }
}

SendFailure SendFailureForThread(const SendFailure failure, const bool trusted_peer) {
  return failure == SendFailure::RelayRateLimited && trusted_peer ? SendFailure::RelayBusy : failure;
}

ShareWord ReceiveFailureWord(const ReceiveFailure reason) {
  for (const auto& [value, wire] : kReceiveWire) {
    if (value == reason) {
      return wire;
    }
  }
  return "decode_failed";
}

const char* ReceiveFailureWire(const ReceiveFailure reason) {
  return ReceiveFailureWord(reason).c_str();
}

ShareWord SendFailureWord(const SendFailure failure) {
  switch (failure) {
    case SendFailure::Unknown:
      return "unknown";
    case SendFailure::MeshOff:
      return "mesh_off";
    case SendFailure::NoAddress:
      return "no_address";
    case SendFailure::BackingOff:
      return "backing_off";
    case SendFailure::DialBusy:
      return "dial_busy";
    case SendFailure::PeerNoAnswer:
      return "peer_no_answer";
    case SendFailure::HandshakeFailed:
      return "handshake_failed";
    case SendFailure::DirectUnconfirmed:
      return "direct_unconfirmed";
    case SendFailure::RelayNotConfigured:
      return "relay_not_configured";
    case SendFailure::RelayBusy:
      return "relay_busy";
    case SendFailure::RelayRateLimited:
      return "relay_rate_limited";
    case SendFailure::RelayUnreachable:
      return "relay_unreachable";
    case SendFailure::RelayRejected:
      return "relay_rejected";
    case SendFailure::RecipientNotOnRelay:
      return "recipient_not_on_relay";
    case SendFailure::PaymentRequired:
      return "payment_required";
  }
  return "unknown";
}

std::optional<ReceiveFailure> ReceiveFailureFromWire(const std::string& wire) {
  for (const auto& [value, name] : kReceiveWire) {
    if (wire == name.view()) {
      return value;
    }
  }
  return std::nullopt;
}

std::optional<SendFailure> SendFailureFromWire(const std::string& wire) {
  static constexpr SendFailure kAll[] = {SendFailure::Unknown,
                                         SendFailure::MeshOff,
                                         SendFailure::NoAddress,
                                         SendFailure::BackingOff,
                                         SendFailure::DialBusy,
                                         SendFailure::PeerNoAnswer,
                                         SendFailure::HandshakeFailed,
                                         SendFailure::DirectUnconfirmed,
                                         SendFailure::RelayNotConfigured,
                                         SendFailure::RelayBusy,
                                         SendFailure::RelayRateLimited,
                                         SendFailure::RelayUnreachable,
                                         SendFailure::RelayRejected,
                                         SendFailure::RecipientNotOnRelay,
                                         SendFailure::PaymentRequired};
  for (const SendFailure failure : kAll) {
    if (wire == SendFailureWord(failure).view()) {
      return failure;
    }
  }
  return std::nullopt;
}

std::string SendFailureRowId(const std::string& message_id, const std::string& reason_wire) {
  return "send-failure-" + message_id + (reason_wire == kGroupMembersMissedWire ? "-group" : "");
}

ThreadMessage MakeSendFailureRow(const std::string& thread_id, const std::string& message_id,
                                 const std::string& reason_wire, const int64_t timestamp_ms) {
  ThreadMessage row;
  row.id = SendFailureRowId(message_id, reason_wire);
  row.thread_id = thread_id;
  row.sender_contact_id = kLocalSelfContactId;
  row.content_type = ChatContentType::System;
  // No stored words: the transcript words the line from its payload, in the reader's language.
  Object detail;
  detail.set("reason", reason_wire);
  detail.set("message_id", message_id);
  Object payload;
  payload.set("control_type", kSendFailureControlType);
  payload.set("detail", DumpJson(detail));
  row.payload_json = DumpJson(payload);
  row.timestamp = timestamp_ms;
  row.delivery = MessageDelivery::Local;
  row.relay_visible = false;
  row.generation = "local";
  return row;
}

std::optional<std::string> SendFailureRowReason(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return std::nullopt;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != kSendFailureControlType) {
    return std::nullopt;
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  if (!detail) {
    return std::nullopt;
  }
  return detail->getString("reason").value_or("");
}

} // namespace pbr
