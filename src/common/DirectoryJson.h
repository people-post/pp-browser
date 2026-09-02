#pragma once

#include "common/DirectoryTypes.h"
#include "common/ValueJson.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

std::string ContactIdKindToString(ContactIdKind kind);
ContactIdKind ContactIdKindFromString(const std::string& value);

pp::common::Value ContactIdsToJson(const std::vector<ContactId>& ids);
void AppendContactIdsFromJson(const pp::common::Object& json, std::vector<ContactId>& ids);

pp::common::Value MultiaddrsToJson(const std::vector<std::string>& multiaddrs);
void ParseMultiaddrsArray(const pp::common::Array& arr, std::vector<std::string>& multiaddrs);

pp::common::Value DirectoryEndpointsToJson(const std::vector<DirectoryEndpoint>& endpoints);
void ParseDirectoryEndpointsArray(const pp::common::Array& arr, std::vector<DirectoryEndpoint>& endpoints);

std::optional<ProfileIconRef> ProfileIconRefFromJson(const pp::common::Object& json);
pp::common::Object ProfileIconRefToJson(const ProfileIconRef& icon);

pp::common::Object DirectoryHitToJson(const DirectoryHit& hit);
DirectoryHit DirectoryHitFromJson(const pp::common::Object& json);

}  // namespace pbr
