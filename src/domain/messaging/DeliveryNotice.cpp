#include "domain/messaging/DeliveryNotice.h"

#include <array>
#include <utility>

namespace pbr {
namespace {

bool Has(const std::string& text, const char* needle) {
  return text.find(needle) != std::string::npos;
}

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

SendFailure ClassifySendFailure(const std::string& error, const bool trusted_peer) {
  if (Has(error, "rate limit")) {
    return trusted_peer ? SendFailure::RelayBusy : SendFailure::RelayRateLimited;
  }
  if (Has(error, "host not running")) {
    return SendFailure::MeshOff;
  }
  if (Has(error, "Empty peer endpoint") || Has(error, "Invalid multiaddr") || Has(error, "missing /p2p/") ||
      Has(error, "Invalid PeerId")) {
    return SendFailure::BadAddress;
  }
  if (Has(error, "not registered") || Has(error, "missing PeerInfo") || Has(error, "Peer-direct endpoint")) {
    return SendFailure::NoAddress;
  }
  if (Has(error, "backoff")) {
    return SendFailure::BackingOff;
  }
  if (Has(error, "Too many concurrent")) {
    return SendFailure::DialBusy;
  }
  if (Has(error, "dial failed") || Has(error, "timed out") || Has(error, "timeout")) {
    return SendFailure::PeerNoAnswer;
  }
  if (Has(error, "stream open failed")) {
    return SendFailure::HandshakeFailed;
  }
  if (Has(error, "Failed to send") || Has(error, "Failed to read") || Has(error, "ack")) {
    return SendFailure::DirectUnconfirmed;
  }
  if (Has(error, "Relay client not configured")) {
    return SendFailure::RelayNotConfigured;
  }
  return SendFailure::Unknown;
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
