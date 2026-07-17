#pragma once

#include "base/people/ContactTypes.h"

#include <nlohmann/json_fwd.hpp>
#include <string>

namespace pbr {

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

nlohmann::json ContactToJson(const Contact& contact);
Contact ContactFromJson(const nlohmann::json& json);

nlohmann::json DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const nlohmann::json& json);

} // namespace pbr
