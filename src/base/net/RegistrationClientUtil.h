#pragma once

#include "base/net/ServiceClients.h"
#include "base/people/IdentityStore.h"

namespace pbr {

Roe<RegistrationResult> FinishRegistrationWithIdentity(IRegistrationClient& registration, IdentityStore& identity,
                                                     const std::string& nickname);

Roe<RegistrationResult> UpdateRegisteredNickname(IRegistrationClient& registration, IdentityStore& identity,
                                                 const std::string& nickname);

} // namespace pbr
