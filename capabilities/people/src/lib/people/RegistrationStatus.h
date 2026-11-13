#pragma once

#include "common/directory/IdentityTypes.h"

#include <cstdint>
#include <string>

namespace pbr {

/** WrongBackend: registered, but not at the backend in use (another one, or recorded by a build that did not say). */
enum class RegistrationStatus { Unregistered, Active, ExpiringSoon, Expired, WrongBackend };

/**
 * Trimmed, no trailing `/`, scheme and host lower-cased: the same text for the same backend. Same rule as
 * NormalizeBackendUrl (foundation/data), which capabilities cannot include.
 */
std::string NormalizeRegistrationBackend(const std::string& url);
/**
 * Whether the identity registered at `backend_url` (normalized here). Empty `backend_url` (no registry configured)
 * never mismatches; an identity with no recorded backend always does.
 */
bool RegisteredAtBackend(const LocalIdentity& identity, const std::string& backend_url);

/** `backend_url`: the registration base URL in use; empty skips the backend check. */
RegistrationStatus ClassifyRegistration(const LocalIdentity& identity, int64_t now_ms = 0,
                                        const std::string& backend_url = {});
/** True if expires within 14 days, already expired/empty, or registered at another backend while registered. */
bool ShouldRenewRegistration(const LocalIdentity& identity, int64_t now_ms = 0, const std::string& backend_url = {});
/** Registered, but the directory entry does not carry the account's KEM key binding yet. */
bool ShouldPublishKemBinding(const LocalIdentity& identity);
std::string RegistrationStatusLabel(RegistrationStatus status);
/** Human label for UI button: "Register on network" vs "Renew registration". */
std::string RegistrationActionLabel(RegistrationStatus status);

/** Mark registration as expired locally (does not Update store). */
void MarkRegistrationExpired(LocalIdentity& identity);

} // namespace pbr
