#pragma once

#include "domain/net/OrgBackendClients.h"
#include "domain/people/IdentityStore.h"
#include "domain/people/RegistrationStatus.h"
#include "common/directory/IdentityTypes.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Apply finish/rotate-style result onto identity fields (does not Update store). */
void ApplyRegistrationResult(LocalIdentity& identity, const RegistrationResult& result);

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
