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

std::optional<std::string> ContactAccountId(const Contact& contact);
std::optional<std::string> ContactRelayUserId(const Contact& contact);

/** First Account / RelayUser id on a directory hit (primary preferred). */
std::optional<std::string> PrimaryAccountIdFromHit(const mesh::DirectoryHit& hit);
std::optional<std::string> PrimaryRelayIdFromHit(const mesh::DirectoryHit& hit);

} // namespace pbr
