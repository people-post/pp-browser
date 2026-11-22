#include "domain/registration/RegistrationClient.h"

#include "lib/people/CardKeys.h"
#include "foundation/error/AppError.h"
#include "lib/net/RegistrationSignPayload.h"
#include "lib/net/RelayOrigins.h"
#include "common/Logger.h"
#include "common/Utilities.h"

#include "common/PbrCompat.h"

namespace pbr {

namespace {

int64_t CardIssuedAt(const RegistrationPublishOpts& publish) {
  return publish.account_card ? publish.account_card->getIf<int64_t>("issued_at").value_or(0) : 0;
}

/** Signs one registry command for the relay `registration` talks to, with the identity's account key (MR007). */
Roe<SignedRelayCommand> SignForRegistry(IRegistrationClient& registration, IdentityStore& identity,
                                        const int64_t timestamp, const RelayCommandBytes& build) {
  return SignRelayCommand(
      registration.Audience(), [&identity](const std::vector<uint8_t>& bytes) { return identity.SignBytes(bytes); },
      timestamp, build);
}

bool IsScopeMismatch(const Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Auth &&
         static_cast<Err::Auth>(error.code) == Err::Auth::ScopeMismatch;
}

/** One revoke of `registration_id` at `registration`'s relay. */
Roe<void> RevokeOnce(IRegistrationClient& registration, IdentityStore& identity, const std::string& account_id,
                     const std::string& registration_id) {
  const int64_t timestamp = util::NowUnixMs();
  auto signed_revoke =
      SignForRegistry(registration, identity, timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
        return BuildAccountRevokeSignBytes(audience, account_id, registration_id, timestamp, ext);
      });
  if (!signed_revoke) {
    return signed_revoke.error();
  }
  return registration.RevokeRegistration(account_id, registration_id, *signed_revoke);
}

/**
 * The account's keys as register/start and register/finish carry them (release-compatibility SIGNED_FORMATS § 2.8):
 * its key card (lib/people/CardKeys.h) — the typed signing and KEM keys, and the KEM binding v2 blob the account signs
 * over them (§ 2.3).
 */
Roe<KeyCard> RegistrationKeys(const LocalIdentity& loaded, IdentityStore& identity) {
  return MakeKeyCard(loaded.account_id, loaded.account_signing_public_key_b64, loaded.kem_public_key_b64,
                     [&identity](const ByteVector& bytes) { return identity.SignBytes(bytes); });
}

} // namespace

RegistrationPublishOpts WithAccountCard(RegistrationPublishOpts publish, const Roe<AccountCard>& card) {
  if (card) {
    publish.account_card = AccountCardToJson(*card);
  }
  return publish;
}

bool ShouldPublishAccountCard(const LocalIdentity& identity, const RegistrationPublishOpts& publish) {
  const int64_t issued_at = CardIssuedAt(publish);
  return identity.registered && issued_at > 0 && issued_at != identity.registered_card_issued_at;
}

void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result) {
  identity.registered = result.success;
  if (!result.llm_token.empty()) {
    identity.brief_llm_api_key = result.llm_token;
    identity.brief_llm_token_expires_at = result.llm_token_expires_at;
    identity.brief_llm_guest_api_key.clear();
  }
  if (!result.expires_at.empty()) {
    identity.registration_expires_at = result.expires_at;
  }
  if (result.success) {
    if (result.account_card_issued_at > 0) {
      identity.registered_card_issued_at = result.account_card_issued_at;
    }
    identity.kem_binding_published = result.kem_bound;
    if (!result.registration_id.empty()) {
      identity.registration_id = result.registration_id;
    }
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

  auto keys = RegistrationKeys(*loaded, identity);
  if (!keys) {
    return keys.error();
  }

  auto start = registration.StartRegistration(keys->signing_public_key_b64, nickname, keys->kem_public_key_b64,
                                              loaded->peer_id, multiaddrs, publish);
  if (!start) {
    return start.error();
  }

  const int64_t timestamp = util::NowUnixMs();
  auto signed_finish =
      SignForRegistry(registration, identity, timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
        return BuildRegistrationSignBytes(audience, start->challenge, keys->signing_public_key_b64,
                                          keys->kem_public_key_b64, timestamp, ext);
      });
  if (!signed_finish) {
    return signed_finish.error();
  }

  auto finished = registration.FinishRegistration(start->challenge, keys->signing_public_key_b64, nickname,
                                                  *signed_finish, keys->kem_public_key_b64, keys->kem_binding_b64,
                                                  loaded->peer_id, multiaddrs, publish);
  if (!finished) {
    return finished;
  }
  // The relay names the inbox it registered by the Account ID it derived from our key (MR001): it must be ours.
  if (finished->success && finished->account_id != loaded->account_id) {
    return Error("Registration answered for another account");
  }
  finished->registered_backend_url = NormalizeRegistrationBackend(registration.BaseUrl());
  if (!finished->account_card_status.empty()) {
    finished->account_card_issued_at = CardIssuedAt(publish); // the relay holds it, or a newer one (stale)
  }
  if (AccountCardStale(*finished)) {
    // Not sent again (the relay keeps the newest): this device's card is older than another device's (X12).
    logging::getLogger("Registration").warning
        << "registration at " << NormalizeRegistrationBackend(registration.BaseUrl())
        << ": the relay keeps a newer account card than this device's (account_card_status stale)";
  }
  return finished;
}

Roe<RegistrationResult> RenewNetworkOnly(IRegistrationClient& registration, IdentityStore& identity,
                                         const std::string& nickname, const std::vector<std::string>& multiaddrs,
                                         const RegistrationPublishOpts& publish) {
  return FinishRegistrationWithIdentity(registration, identity, nickname, multiaddrs, publish);
}

RegistrationJob MakeRegistrationJob(std::shared_ptr<IRegistrationClient> registration, IdentityStore& identity_store,
                                    const LocalIdentity& identity, std::vector<std::string> listen_addrs,
                                    RegistrationPublishOpts publish) {
  RegistrationJob job;
  job.account_id = identity.account_id;
  job.nickname = identity.nickname;
  job.listen_addrs = std::move(listen_addrs);
  job.publish = std::move(publish);
  job.registration = std::move(registration);
  job.identity = &identity_store;
  return job;
}

Roe<RegistrationResult> RunRegistrationJob(const RegistrationJob& job) {
  if (job.registration == nullptr || job.identity == nullptr) {
    return Error("Registration job has no client");
  }
  return RenewNetworkOnly(*job.registration, *job.identity, job.nickname, job.listen_addrs, job.publish);
}

Roe<LocalIdentity> PersistRenewal(IdentityStore& identity, const RegistrationResult& result,
                                  const std::string& expected_account_id, const RegistrationHome& home) {
  // Read again, under the lock: the network half took a while, and only the registration fields are ours to change.
  if (expected_account_id.empty()) {
    return Error("Registration renewal not stored: no account to store it for");
  }
  if (const std::string preferred = home ? home() : std::string();
      !preferred.empty() && !result.registered_backend_url.empty() &&
      !SameRelay(preferred, result.registered_backend_url)) {
    // Made at the relay that was preferred when it began: the identity's registration is the new one's now.
    return Error("Registration renewal not stored: made at " +
                 NormalizeRegistrationBackend(result.registered_backend_url) + ", no longer the preferred relay");
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
                                                const RegistrationPublishOpts& publish, const RegistrationHome& home) {
  auto loaded = identity.Get(); // the account the registration runs for
  if (!loaded) {
    return loaded.error();
  }
  auto result = RenewNetworkOnly(registration, identity, nickname, multiaddrs, publish);
  if (!result) {
    return result.error();
  }
  return PersistRenewal(identity, *result, loaded->account_id, home);
}

Roe<std::optional<RegistrationResult>> MaybeAutoRenewNetworkOnly(IRegistrationClient& registration,
                                                                 IdentityStore& identity, const bool auto_renew_enabled,
                                                                 const RegistrationPublishOpts& publish) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  // A registration from before KEM key bindings publishes one now: peers take the KEM key only with it.
  // A registration made at another backend (or by a build that did not record one) renews against this one. A card
  // newer than the one the relay took goes too: the relay reads our relays and floor from it (MR002 / MR005).
  const bool due = ShouldRenewRegistration(*loaded, 0, registration.BaseUrl());
  if (!ShouldPublishKemBinding(*loaded) && !ShouldPublishAccountCard(*loaded, publish) &&
      (!due || !auto_renew_enabled)) {
    return std::optional<RegistrationResult>{};
  }
  auto result = RenewNetworkOnly(registration, identity, loaded->nickname, {}, publish);
  if (!result) {
    return result.error();
  }
  return std::optional<RegistrationResult>(std::move(*result));
}

Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled, const RegistrationPublishOpts& publish,
                                     const RegistrationHome& home) {
  auto loaded = identity.Get(); // the account the renewal runs for
  if (!loaded) {
    return loaded.error();
  }
  auto result = MaybeAutoRenewNetworkOnly(registration, identity, auto_renew_enabled, publish);
  if (!result) {
    return result.error();
  }
  if (!*result) {
    return false;
  }
  if (auto applied = PersistRenewal(identity, **result, loaded->account_id, home); !applied) {
    return applied.error();
  }
  return true;
}

bool IsRegistrationRefused(const Error& error) {
  return AppError::CategoryOf(error) == ErrorCategory::Network &&
         error.code == static_cast<int32_t>(Err::Network::HttpRejected);
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
  auto signed_update =
      SignForRegistry(registration, identity, timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
        return BuildProfileUpdateSignBytes(audience, loaded->account_id, nickname, timestamp, ext);
      });
  if (!signed_update) {
    return signed_update.error();
  }
  return registration.UpdateNickname(nickname, *signed_update, loaded->account_id);
}

Roe<void> RevokeRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                         const std::string& registration_id) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->account_id.empty()) {
    return {};
  }
  if (!registration_id.empty()) {
    auto revoked = RevokeOnce(registration, identity, loaded->account_id, registration_id);
    if (revoked || !IsScopeMismatch(revoked.error())) {
      return revoked;
    }
  }
  // No id kept, or the relay holds a newer registration: renew (its finish answers the current id), then revoke that
  // one, once (MR007 item 5).
  auto renewed = RenewNetworkOnly(registration, identity, loaded->nickname);
  if (!renewed) {
    return renewed.error();
  }
  if (renewed->registration_id.empty()) {
    return Error("Registration revoke: the relay answered no registration_id");
  }
  return RevokeOnce(registration, identity, loaded->account_id, renewed->registration_id);
}

Roe<LlmTokenRenewal> RenewBriefLlmTokenWithIdentity(IRegistrationClient& registration, IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (loaded->account_id.empty() || loaded->brief_llm_api_key.empty()) {
    return AppError::Auth(Err::Auth::NotRegistered, "No Brief LLM token to renew");
  }
  if (!registration.TakesBriefLlmKey()) {
    return Error("Brief LLM token renew: this relay is not the org's");
  }
  const std::string account_id = loaded->account_id;
  const int64_t timestamp = util::NowUnixMs();
  auto signed_renew =
      SignForRegistry(registration, identity, timestamp, [&](const std::string& audience, const pp::ExtArea& ext) {
        return BuildLlmTokenRenewSignBytes(audience, account_id, timestamp, ext);
      });
  if (!signed_renew) {
    return signed_renew.error();
  }
  auto renewed = registration.RenewLlmToken(account_id, *signed_renew);
  if (!renewed) {
    return renewed.error();
  }
  // Into the account it was asked for: the registered slot takes the new token (a guest key beside it goes, as at
  // register/finish).
  bool other_account = false;
  auto kept = identity.Modify([&](LocalIdentity& now) {
    if (now.account_id != account_id) {
      other_account = true;
      return false;
    }
    now.brief_llm_api_key = renewed->llm_token;
    now.brief_llm_token_expires_at = renewed->llm_token_expires_at;
    now.brief_llm_guest_api_key.clear();
    return true;
  });
  if (!kept) {
    return kept.error();
  }
  if (other_account) {
    return Error("Brief LLM token not kept: the identity is another account now");
  }
  return renewed;
}

} // namespace pbr
