#include "feature/conversations/RegistrationClient.h"

#include "domain/net/RegistrationSignPayload.h"
#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {

void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result) {
  identity.registered = result.success;
  if (!result.relay_user_id.empty()) {
    identity.relay_user_id = result.relay_user_id;
  }
  if (!result.llm_api_key.empty()) {
    identity.brief_llm_api_key = result.llm_api_key;
    identity.brief_llm_guest_api_key.clear();
  }
  if (!result.expires_at.empty()) {
    identity.registration_expires_at = result.expires_at;
  }
  if (result.initiation_floor_present) {
    identity.initiation_floor = result.initiation_floor;
  }
}

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                       const std::string& nickname,
                                                       const std::vector<std::string>& multiaddrs,
                                                       const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->account_signing_public_key_b64.empty() || loaded->account_id.empty()) {
    return Error("account signing key / Account ID not set");
  }
  if (loaded->kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 not set");
  }

  auto start = registration.StartRegistration(loaded->account_signing_public_key_b64, nickname, "ml-dsa-65",
                                              loaded->kem_public_key_b64, loaded->peer_id, multiaddrs, publish);
  if (!start) {
    return start.error();
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildRegistrationSignBytes(start->challenge, loaded->account_signing_public_key_b64,
                                                     loaded->kem_public_key_b64, start->signature_alg, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build registration sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.FinishRegistration(start->challenge, loaded->account_signing_public_key_b64, nickname,
                                         *signature, timestamp, start->signature_alg, loaded->kem_public_key_b64,
                                         loaded->peer_id, multiaddrs, loaded->initiation_floor, publish);
}

Roe<LocalIdentity> FinishAndPersistRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                                const std::string& nickname,
                                                const std::vector<std::string>& multiaddrs,
                                                const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }

  auto result = FinishRegistrationWithIdentity(registration, identity, nickname, multiaddrs, publish);
  if (!result) {
    return result.error();
  }

  LocalIdentity updated = *loaded;
  ApplyRegistrationResult(updated, *result);
  if (auto saved = identity.Update(updated); !saved) {
    return saved.error();
  }
  return updated;
}

Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!ShouldRenewRegistration(*loaded)) {
    return false;
  }
  if (!auto_renew_enabled) {
    return false;
  }

  auto applied = FinishAndPersistRegistration(registration, identity, loaded->nickname);
  if (!applied) {
    return applied.error();
  }
  return true;
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
