#include "domain/registration/RegistrationClient.h"

#include "foundation/crypto/KemKeyBinding.h"
#include "lib/net/RegistrationSignPayload.h"
#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {

void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result) {
  identity.registered = result.success;
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
  if (result.success) {
    identity.kem_binding_published = result.kem_bound;
    if (!result.registered_backend_url.empty()) {
      identity.registered_backend_url = result.registered_backend_url;
    }
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

  // The account binds its KEM key to itself; the directory hands the binding to peers with the key.
  const auto binding_bytes = KemBindingSignBytes(loaded->account_signing_public_key_b64, loaded->kem_public_key_b64);
  if (binding_bytes.empty()) {
    return Error("Failed to build KEM key binding");
  }
  auto kem_binding = identity.SignBytes(binding_bytes);
  if (!kem_binding) {
    return kem_binding.error();
  }

  auto finished = registration.FinishRegistration(
      start->challenge, loaded->account_signing_public_key_b64, nickname, *signature, timestamp, start->signature_alg,
      loaded->kem_public_key_b64, *kem_binding, loaded->peer_id, multiaddrs, loaded->initiation_floor, publish);
  if (!finished) {
    return finished;
  }
  // The relay names the inbox it registered by the Account ID it derived from our key (MR001): it must be ours.
  if (finished->success && finished->account_id != loaded->account_id) {
    return Error("Registration answered for another account");
  }
  finished->registered_backend_url = NormalizeRegistrationBackend(registration.BaseUrl());
  return finished;
}

Roe<RegistrationResult> RenewNetworkOnly(IRegistrationClient& registration, IdentityStore& identity,
                                         const std::string& nickname, const std::vector<std::string>& multiaddrs,
                                         const RegistrationPublishOpts& publish) {
  return FinishRegistrationWithIdentity(registration, identity, nickname, multiaddrs, publish);
}

Roe<LocalIdentity> PersistRenewal(IdentityStore& identity, const RegistrationResult& result,
                                  const std::string& expected_account_id) {
  // Read again, under the lock: the network half took a while, and only the registration fields are ours to change.
  if (expected_account_id.empty()) {
    return Error("Registration renewal not stored: no account to store it for");
  }
  bool other_account = false;
  auto applied = identity.Modify([&](LocalIdentity& stored) {
    if (stored.account_id != expected_account_id) {
      other_account = true;
      return false;
    }
    ApplyRegistrationResult(stored, result);
    return true;
  });
  if (!applied) {
    return applied.error();
  }
  if (other_account) {
    return Error("Registration renewal not stored: the identity is another account now");
  }
  return applied;
}

Roe<LocalIdentity> FinishAndPersistRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                                const std::string& nickname, const std::vector<std::string>& multiaddrs,
                                                const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get(); // the account the registration runs for
  if (!loaded) {
    return loaded.error();
  }
  auto result = RenewNetworkOnly(registration, identity, nickname, multiaddrs, publish);
  if (!result) {
    return result.error();
  }
  return PersistRenewal(identity, *result, loaded->account_id);
}

Roe<std::optional<RegistrationResult>> MaybeAutoRenewNetworkOnly(IRegistrationClient& registration,
                                                                 IdentityStore& identity,
                                                                 const bool auto_renew_enabled) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  // A registration from before KEM key bindings publishes one now: peers take the KEM key only with it.
  // A registration made at another backend (or by a build that did not record one) renews against this one.
  const bool due = ShouldRenewRegistration(*loaded, 0, registration.BaseUrl());
  if (!ShouldPublishKemBinding(*loaded) && (!due || !auto_renew_enabled)) {
    return std::optional<RegistrationResult>{};
  }
  auto result = RenewNetworkOnly(registration, identity, loaded->nickname);
  if (!result) {
    return result.error();
  }
  return std::optional<RegistrationResult>(std::move(*result));
}

Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled) {
  auto loaded = identity.Get(); // the account the renewal runs for
  if (!loaded) {
    return loaded.error();
  }
  auto result = MaybeAutoRenewNetworkOnly(registration, identity, auto_renew_enabled);
  if (!result) {
    return result.error();
  }
  if (!*result) {
    return false;
  }
  if (auto applied = PersistRenewal(identity, **result, loaded->account_id); !applied) {
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
  if (!loaded->registered || loaded->account_id.empty()) {
    return Error("Not registered on the relay");
  }

  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildProfileUpdateSignBytes(loaded->account_id, nickname, timestamp);
  if (sign_bytes.empty()) {
    return Error("Failed to build profile sign bytes");
  }

  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }

  return registration.UpdateNickname(nickname, *signature, timestamp, loaded->account_id);
}

Roe<void> RevokeRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->account_id.empty()) {
    return {};
  }
  const int64_t timestamp = util::NowUnixMs();
  const auto sign_bytes = BuildAccountRevokeSignBytes(loaded->account_id, timestamp);
  auto signature = identity.SignBytes(sign_bytes);
  if (!signature) {
    return signature.error();
  }
  return registration.RevokeRegistration(loaded->account_id, *signature, timestamp);
}

} // namespace pbr
