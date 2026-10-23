#pragma once

#include "common/directory/IdentityTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

enum class RegistrationStatus { Unregistered, Active, ExpiringSoon, Expired };

RegistrationStatus ClassifyRegistration(const LocalIdentity& identity, int64_t now_ms = 0);
/** True if expires within 14 days or already expired/empty while registered. */
bool ShouldRenewRegistration(const LocalIdentity& identity, int64_t now_ms = 0);
std::string RegistrationStatusLabel(RegistrationStatus status);
/** Human label for UI button: "Register on network" vs "Renew registration". */
std::string RegistrationActionLabel(RegistrationStatus status);

/** Mark registration as expired locally (does not Update store). */
void MarkRegistrationExpired(LocalIdentity& identity);

} // namespace pbr
