#include "lib/net/RelayAudience.h"

#include "common/ValueJson.h"
#include "foundation/error/AppError.h"

namespace pbr {

Roe<std::string> FixedRelayAudience::PeerId() {
  if (peer_id_.empty()) {
    return AppError::Network(Err::Network::RelayIdentityUnavailable, "no relay PeerId given");
  }
  return peer_id_;
}

void FixedRelayAudience::Refused(const std::string& code) {
  if (on_refused_) {
    on_refused_(code);
  }
}

Roe<SignedRelayCommand> SignRelayCommand(IRelayAudience* audience, const RelayAuthSigner& signer,
                                         const int64_t timestamp, const RelayCommandBytes& build) {
  if (!signer) {
    return Error("Relay auth signer not configured");
  }
  if (!audience) {
    return AppError::Network(Err::Network::RelayIdentityUnavailable, "relay audience not configured");
  }
  auto peer_id = audience->PeerId();
  if (!peer_id) {
    return peer_id.error();
  }
  const std::vector<uint8_t> bytes = build(*peer_id);
  if (bytes.empty()) {
    return Error("Failed to build relay command sign bytes");
  }
  auto signature = signer(bytes);
  if (!signature) {
    return signature.error();
  }
  return SignedRelayCommand{.audience = std::move(*peer_id), .timestamp = timestamp, .signature = *signature};
}

void PutSignedRelayCommand(Object& body, const SignedRelayCommand& command) {
  body.set("audience", command.audience);
  body.set("timestamp", command.timestamp);
  body.set("signature", command.signature);
}

std::string RelayRefusalCode(const std::string& body) {
  if (auto json = TryParseObject(body)) {
    return json->getString("code").value_or("");
  }
  return {};
}

std::optional<Error> NoteRelayRefusal(IRelayAudience* audience, const long status, const std::string& body) {
  const std::string code = RelayRefusalCode(body);
  if (status == 401 && code == kWrongAudienceCode) {
    if (audience) {
      audience->Refused(code);
    }
    return AppError::Auth(Err::Auth::WrongAudience, "Relay refused the command: signed for another relay");
  }
  if ((status == 403 || status == 404) && (code == kNotRegisteredCode || code == kRegistrationExpiredCode)) {
    if (audience) {
      audience->Refused(code);
    }
    return AppError::Auth(Err::Auth::NotRegistered, "Relay refused the command: " + code);
  }
  return std::nullopt;
}

} // namespace pbr
