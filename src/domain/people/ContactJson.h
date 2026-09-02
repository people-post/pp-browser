#pragma once

#include "domain/people/ContactTypes.h"
#include "common/directory/DirectoryJson.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

std::string TrustLevelToString(TrustLevel level);
TrustLevel TrustLevelFromString(const std::string& value);

pp::common::Object ContactToJson(const Contact& contact);
Contact ContactFromJson(const pp::common::Object& json);

}  // namespace pbr
