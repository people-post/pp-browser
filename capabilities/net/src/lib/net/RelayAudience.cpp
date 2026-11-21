#include "lib/net/RelayAudience.h"

#include "common/ValueJson.h"
#include "foundation/error/AppError.h"
#include "lib/net/RelaySignBytes.h"
#include "lib/people/CardKeys.h"

#include "foundation/crypto/SignedBytes.h"

#include <algorithm>
#include <iterator>

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
                                         const int64_t timestamp, const RelayCommandBytes& build,
                                         const pp::ExtArea& ext) {
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
  const std::vector<uint8_t> bytes = build(*peer_id, ext);
  const int sign_version = RelaySignVersionOf(bytes);
  if (bytes.empty() || sign_version < 0) {
    return Error("Failed to build relay command sign bytes");
  }
  if (!RelayCallBytesEndWith(bytes, ext)) {
    return Error("Relay command sign bytes do not end with the extension area sent");
  }
  auto signature = signer(bytes);
  if (!signature) {
    return signature.error();
  }
  auto typed = TypedSignatureB64(*signature);
  if (!typed) {
    return typed.error();
  }
  return SignedRelayCommand{.sign_version = sign_version,
                            .audience = std::move(*peer_id),
                            .timestamp = timestamp,
                            .ext = ext,
                            .signature = std::move(*typed)};
}

void PutSignedRelayCommand(Object& body, const SignedRelayCommand& command) {
  body.set("sign_version", static_cast<int64_t>(command.sign_version));
  body.set("audience", command.audience);
  body.set("timestamp", command.timestamp);
  WriteExtB64(body, command.ext);
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
  if (status == 426 && code == kClientUpgradeRequiredCode) {
    if (audience) {
      audience->Refused(code);
    }
    return AppError::Auth(Err::Auth::ClientUpgradeRequired,
                          "Relay refused the command: this app is too old for it (client_upgrade_required)");
  }
  if (status == 400 && (code == kRelayUpgradeRequiredCode || code == kUnsupportedSignVersionCode)) {
    if (audience) {
      audience->Refused(kRelayUpgradeRequiredCode);
    }
    return AppError::Auth(Err::Auth::RelayUpgradeRequired,
                          "Relay refused the command: the relay is older than this app (" + code + ")");
  }
  if (status == 400 && std::find(std::begin(kAccountCardRefusalCodes), std::end(kAccountCardRefusalCodes), code) !=
                           std::end(kAccountCardRefusalCodes)) {
    return AppError::Auth(Err::Auth::AccountCardRefused, "Relay refused the account card (" + code + ")");
  }
  if ((status == 403 || status == 404) && (code == kNotRegisteredCode || code == kRegistrationExpiredCode)) {
    if (audience) {
      audience->Refused(code);
    }
    return AppError::Auth(Err::Auth::NotRegistered, "Relay refused the command: " + code);
  }
  return std::nullopt;
}

namespace {
bool IsAuth(const Error& error, const Err::Auth code) {
  return AppError::CategoryOf(error) == ErrorCategory::Auth && static_cast<Err::Auth>(error.code) == code;
}
} // namespace

bool IsClientUpgradeRequired(const Error& error) {
  return IsAuth(error, Err::Auth::ClientUpgradeRequired);
}

bool IsRelayUpgradeRequired(const Error& error) {
  return IsAuth(error, Err::Auth::RelayUpgradeRequired);
}

bool IsPermanentRelayRefusal(const Error& error) {
  return IsClientUpgradeRequired(error) || IsRelayUpgradeRequired(error) || IsAuth(error, Err::Auth::WrongAudience);
}

} // namespace pbr
