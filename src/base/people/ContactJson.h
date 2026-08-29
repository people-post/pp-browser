#pragma once

#include "base/people/ContactTypes.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

pp::common::Object ContactToJson(const Contact& contact);
Contact ContactFromJson(const pp::common::Object& json);

pp::common::Object DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const pp::common::Object& json);

} // namespace pbr
