#pragma once

#include "mesh/types/DirectoryTypes.h"
#include "common/ValueJson.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

std::string ContactIdKindToString(mesh::ContactIdKind kind);
mesh::ContactIdKind ContactIdKindFromString(const std::string& value);

pp::common::Value ContactIdsToJson(const std::vector<mesh::ContactId>& ids);
void AppendContactIdsFromJson(const pp::common::Object& json, std::vector<mesh::ContactId>& ids);

pp::common::Value MultiaddrsToJson(const std::vector<std::string>& multiaddrs);
void ParseMultiaddrsArray(const pp::common::Array& arr, std::vector<std::string>& multiaddrs);

pp::common::Value DirectoryEndpointsToJson(const std::vector<mesh::DirectoryEndpoint>& endpoints);
void ParseDirectoryEndpointsArray(const pp::common::Array& arr, std::vector<mesh::DirectoryEndpoint>& endpoints);

std::optional<mesh::ProfileIconRef> ProfileIconRefFromJson(const pp::common::Object& json);
pp::common::Object ProfileIconRefToJson(const mesh::ProfileIconRef& icon);

pp::common::Object DirectoryHitToJson(const mesh::DirectoryHit& hit);
mesh::DirectoryHit DirectoryHitFromJson(const pp::common::Object& json);

}  // namespace pbr
