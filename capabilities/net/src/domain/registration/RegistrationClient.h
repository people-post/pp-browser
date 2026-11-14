#pragma once

#include "lib/net/OrgBackendClients.h"
#include "lib/people/AccountCard.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/RegistrationStatus.h"
#include "common/directory/IdentityTypes.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** `publish` carrying our account card for register/finish (MR002 / MR005); as it is when `card` is an error. */
RegistrationPublishOpts WithAccountCard(RegistrationPublishOpts publish, const Roe<AccountCard>& card);
/** Registered, and `publish` carries a card other than the one the relay last answered for: it goes again. */
bool ShouldPublishAccountCard(const LocalIdentity& identity, const RegistrationPublishOpts& publish);

/** Apply finish/rotate-style result onto identity fields (does not Update store). */
void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result);

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                       const std::string& nickname,
                                                       const std::vector<std::string>& multiaddrs = {},
                                                       const RegistrationPublishOpts& publish = {});

/**
 * The network half of FinishAndPersistRegistration: start, sign, finish. Nothing is stored; the result goes to
 * PersistRenewal. Reads the identity (and signs with its key) under the store's own lock, so it may run off the UI
 * thread.
 */
Roe<RegistrationResult> RenewNetworkOnly(IRegistrationClient& registration, IdentityStore& identity,
                                         const std::string& nickname, const std::vector<std::string>& multiaddrs = {},
                                         const RegistrationPublishOpts& publish = {});

/**
 * The storing half: in one read-modify-write under the store's lock (IdentityStore::Modify), applies only the
 * registration fields of `result` (ApplyRegistrationResult) to the identity as stored now. `expected_account_id`:
 * the account the network half ran for; another account stored now (the profile changed meanwhile), or an empty
 * `expected_account_id`, is refused and nothing is written. Returns the applied identity.
 */
Roe<LocalIdentity> PersistRenewal(IdentityStore& identity, const RegistrationResult& result,
                                  const std::string& expected_account_id);

/**
 * What a registration the user asked for needs to run its network half off the UI thread: everything the UI thread read
 * (account, nickname, advertised addresses, the account card) by value, and the two stores it talks to. The
 * registration client is held shared, so a relay change that replaces it meanwhile leaves the run its own; the identity
 * store must outlive the run (a teardown waits for running worker tasks before it destroys it).
 */
struct RegistrationJob {
  std::string account_id;
  std::string nickname;
  std::vector<std::string> listen_addrs;
  RegistrationPublishOpts publish;
  std::shared_ptr<IRegistrationClient> registration;
  IdentityStore* identity = nullptr;
};

/** The job for `identity` (the identity as stored once its new nickname was saved); the inputs as given. */
RegistrationJob MakeRegistrationJob(std::shared_ptr<IRegistrationClient> registration, IdentityStore& identity_store,
                                    const LocalIdentity& identity, std::vector<std::string> listen_addrs,
                                    RegistrationPublishOpts publish);

/** The job's network half (RenewNetworkOnly with its inputs); nothing is stored. May run off the UI thread. */
Roe<RegistrationResult> RunRegistrationJob(const RegistrationJob& job);

/** Finish registration + Identity.Update with ApplyRegistrationResult: RenewNetworkOnly, then PersistRenewal. */
Roe<LocalIdentity> FinishAndPersistRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                                const std::string& nickname,
                                                const std::vector<std::string>& multiaddrs = {},
                                                const RegistrationPublishOpts& publish = {});

/**
 * If ShouldRenew (incl. registered at another backend than `registration`'s) and auto_renew_enabled, finish+persist registration.
 * Also, whatever the preference, when the KEM binding or `publish`'s account card is not published yet
 * (ShouldPublishKemBinding, ShouldPublishAccountCard). Returns true if renewed, false if nothing was due.
 */
Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled, const RegistrationPublishOpts& publish = {});

/**
 * The network half of MaybeAutoRenewRegistration: nullopt when nothing is due (same rule), else the finished
 * registration for PersistRenewal. Nothing is stored.
 */
Roe<std::optional<RegistrationResult>> MaybeAutoRenewNetworkOnly(IRegistrationClient& registration,
                                                                 IdentityStore& identity, bool auto_renew_enabled,
                                                                 const RegistrationPublishOpts& publish = {});

/**
 * The server refused the registration itself (RegistrationHttpStatusError: a 4xx but 408 / 429), as opposed to a
 * failure that may pass (network, timeout, 5xx, 408, 429, or an untyped local error).
 */
bool IsRegistrationRefused(const Error& error);

Roe<RegistrationResult> UpdateRegisteredNickname(IRegistrationClient& registration, IdentityStore& identity,
                                                 const std::string& nickname);
/**
 * Remove this account's registration `registration_id` from `registration`'s relay (#328, MR007 item 5: the id its
 * `register/finish` answered). Nothing to do (success) when the identity never registered. With no id kept, or one the
 * relay answers `registration_mismatch` (a newer registration there), it renews first and revokes the id that renewal
 * answers, once. Does not touch the local identity: the caller is about to wipe it, or the relay leaves our list.
 */
Roe<void> RevokeRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                         const std::string& registration_id);

} // namespace pbr
