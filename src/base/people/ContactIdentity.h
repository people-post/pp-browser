#pragma once

#include "base/people/ContactTypes.h"

#include <optional>
#include <string>

namespace pbr {

/** First matching id of kind (primary preferred). */
std::string PrimaryIdOfKind(const Contact& contact, ContactIdKind kind);

std::optional<std::string> ContactAccountId(const Contact& contact);
std::optional<std::string> ContactRelayUserId(const Contact& contact);

/** First Account / RelayUser id on a directory hit (primary preferred). */
std::optional<std::string> PrimaryAccountIdFromHit(const DirectoryHit& hit);
std::optional<std::string> PrimaryRelayIdFromHit(const DirectoryHit& hit);

} // namespace pbr
