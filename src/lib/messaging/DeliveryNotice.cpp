#include "lib/messaging/DeliveryNotice.h"

#include "foundation/error/AppError.h"

#include <array>
#include <utility>

namespace pbr {
namespace {

constexpr std::array<std::pair<ReceiveFailure, const char*>, 15> kReceiveWire = {{
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
    return static_cast<Err::Auth>(error.code) == Err::Auth::RateLimited ? SendFailure::RelayRateLimited
                                                                         : SendFailure::RelayRejected;
  case ErrorCategory::Config:
    return SendFailure::RelayNotConfigured;
  case ErrorCategory::Network:
    return static_cast<Err::Network>(error.code) == Err::Network::HttpError ? SendFailure::RelayRejected
                                                                             : SendFailure::RelayUnreachable;
  default:
    return SendFailure::Unknown;
  }
}

SendFailure SendFailureForThread(const SendFailure failure, const bool trusted_peer) {
  return failure == SendFailure::RelayRateLimited && trusted_peer ? SendFailure::RelayBusy : failure;
}

const char* ReceiveFailureWire(const ReceiveFailure reason) {
  for (const auto& [value, wire] : kReceiveWire) {
    if (value == reason) {
      return wire;
    }
  }
  return "decode_failed";
}

std::optional<ReceiveFailure> ReceiveFailureFromWire(const std::string& wire) {
  for (const auto& [value, name] : kReceiveWire) {
    if (wire == name) {
      return value;
    }
  }
  return std::nullopt;
}

} // namespace pbr
