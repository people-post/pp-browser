#pragma once

#include "lib/people/ContactTypes.h"
#include "common/MeshAlias.h"

#include <optional>
#include <string>

namespace pbr {

/** True when value is a full Account ID (`account:…`, M002). */
bool IsAccountIdentityValue(const std::string& value);

/** First matching id of kind (primary preferred). */
std::string PrimaryIdOfKind(const Contact& contact, mesh::ContactIdKind kind);

/** The id every screen seeds a contact's letter-avatar colour with: account, else peer, else the contact id. */
std::string ContactAvatarStableId(const Contact& contact);

std::optional<std::string> ContactAccountId(const Contact& contact);

/** The Account ID on a directory hit (its `account_id`, else its primary Account id). */
std::optional<std::string> PrimaryAccountIdFromHit(const mesh::DirectoryHit& hit);

} // namespace pbr
