#pragma once

#include "common/MeshConfig.h"
#include "common/ServiceEndpoints.h"
#include "common/Value.h"
#include "foundation/platform/BackendTable.h"

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/** The config file's `config_version` this build writes; a newer one is refused (both apps). */
inline constexpr int kConfigFileVersion = 1;

// The config file's sections both apps read (capability-repos CR028): the mesh, the service endpoints (registration,
// relay, directory) and a custom backend's services. pp-node reads them with no other part of the desktop's config;
// ConfigJson.h has the rest.

pp::common::Object ServiceEndpointToObject(const ServiceEndpointConfig& endpoint);
void ServiceEndpointFromObject(const pp::common::Object& object, ServiceEndpointConfig& endpoint);

pp::common::Object DirectoryConfigToObject(const DirectoryConfig& config);
void DirectoryConfigFromObject(const pp::common::Object& object, DirectoryConfig& config);

pp::common::Object MeshCapabilitiesToObject(const mesh::MeshCapabilities& caps);
void MeshCapabilitiesFromObject(const pp::common::Object& object, mesh::MeshCapabilities& caps);

pp::common::Object MeshDhtConfigToObject(const mesh::MeshDhtConfig& config);
void MeshDhtConfigFromObject(const pp::common::Object& object, mesh::MeshDhtConfig& config);

pp::common::Object MeshConfigToObject(const MeshConfig& config);
void MeshConfigFromObject(const pp::common::Object& object, MeshConfig& config);

/**
 * The services a custom backend's file names, read straight from the file's object before any default is merged: the
 * `relay`, `registration` and `directory` keys, `relay.extra_base_urls` (the seed relays) and `backend.services`. What the
 * file does not name stays empty (profile-model PM002: never a default of another backend).
 */
CustomServices CustomServicesFromObject(const pp::common::Object& root);

/** `backend.services`: only the services the file names. */
void BackendServicesFromObject(const pp::common::Object& object, CustomServices& services);
pp::common::Object BackendServicesToObject(const CustomServices& services);

} // namespace pbr
