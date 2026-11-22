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

/**
 * A directory row's key fields, as a key card carries them (docs/contracts/ACCOUNT_CARD.md § Key card): typed keys and
 * the KEM binding v2 blob. Read for their shape only, by the mesh's rule (pp-cpp-mesh `MeshNodeHitCodec.h`,
 * `DirectoryKeyShape`; its docs/DIRECTORY.md), as far as a carrier can go in the receiver's order (version, parse,
 * algorithms): a malformed field, or a binding older than v2, is dropped (absent: the row carries no usable keys); one
 * of an algorithm or binding version this build does not know is kept as received and flags `keys_update_needed` (the
 * account needs a newer app here, its keys are not bad). Nothing is verified: that is the key card's (VerifyKeyCard).
 */
struct DirectoryRowKeyFields {
  std::optional<std::string> signing_public_key_b64;
  std::optional<std::string> kem_public_key_b64;
  std::optional<std::string> kem_binding_b64;
  bool keys_update_needed = false;
};
DirectoryRowKeyFields ReadDirectoryRowKeys(const pp::common::Object& row);

pp::common::Object DirectoryHitToJson(const mesh::DirectoryHit& hit);
mesh::DirectoryHit DirectoryHitFromJson(const pp::common::Object& json);

} // namespace pbr
