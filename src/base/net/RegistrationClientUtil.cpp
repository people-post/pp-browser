#include "base/net/RegistrationClientUtil.h"

#include "common/Utilities.h"
#include "base/net/RegistrationSignPayload.h"

namespace pbr {

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                       const std::string& nickname) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }

  auto start = registration.StartRegistration(loaded->public_key_b64, nickname);
  if (!start) {
    return start.error();
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes =
      BuildRegistrationSignBytes(start->challenge, loaded->public_key_b64, start->signature_alg, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build registration sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.FinishRegistration(start->challenge, loaded->public_key_b64, nickname, *signature, timestamp,
                                       start->signature_alg);
}

Roe<RegistrationResult> UpdateRegisteredNickname(IRegistrationClient& registration, IdentityStore& identity,
                                                 const std::string& nickname) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->relay_user_id.empty()) {
    return Error("relay_user_id not set");
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildProfileUpdateSignBytes(loaded->relay_user_id, nickname, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build profile sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.UpdateNickname(nickname, *signature, timestamp, loaded->relay_user_id);
}

} // namespace pbr
