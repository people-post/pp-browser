#pragma once

#include "base/people/ContactTypes.h"
#include "common/PbrCompat.h"

#include <string>

namespace pbr {

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

Object ContactToJson(const Contact& contact);
Contact ContactFromJson(const Object& json);

Object DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const Object& json);

} // namespace pbr
