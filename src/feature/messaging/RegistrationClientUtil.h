#pragma once

#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"
#include "common/directory/IdentityTypes.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

enum class RegistrationStatus { Unregistered, Active, ExpiringSoon, Expired };

RegistrationStatus ClassifyRegistration(const LocalIdentity& identity, int64_t now_ms = 0);
/** True if expires within 14 days or already expired/empty while registered. */
bool ShouldRenewRegistration(const LocalIdentity& identity, int64_t now_ms = 0);
std::string RegistrationStatusLabel(RegistrationStatus status);
/** Human label for UI button: "Register on network" vs "Renew registration". */
std::string RegistrationActionLabel(RegistrationStatus status);

/** Apply finish/rotate-style result onto identity fields (does not Update store). */
void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result);

/** Mark registration as expired locally (does not Update store). */
void MarkRegistrationExpired(LocalIdentity& identity);

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                       const std::string& nickname,
                                                       const std::vector<std::string>& multiaddrs = {},
                                                       const RegistrationPublishOpts& publish = {});

/** Finish registration + Identity.Update with ApplyRegistrationResult. Returns applied identity. */
Roe<LocalIdentity> FinishAndPersistRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                                const std::string& nickname,
                                                const std::vector<std::string>& multiaddrs = {},
                                                const RegistrationPublishOpts& publish = {});

/**
 * If ShouldRenew and auto_renew_enabled, finish+persist registration.
 * Returns true if renewed, false if no renew needed or preference off.
 */
Roe<bool> MaybeAutoRenewRegistration(IRegistrationClient& registration, IdentityStore& identity,
                                     bool auto_renew_enabled);

Roe<RegistrationResult> UpdateRegisteredNickname(IRegistrationClient& registration, IdentityStore& identity,
                                                 const std::string& nickname);

} // namespace pbr
