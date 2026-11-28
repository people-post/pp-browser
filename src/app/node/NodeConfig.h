#pragma once

#include "common/MeshConfig.h"
#include "common/ServiceEndpoints.h"
#include "common/Value.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * What pp-node reads of its config file (capability-repos CR028): `mesh`, `registration`, `data_dir`,
 * `relay.content_sha256` and `node`, the same keys the desktop app's file has, so a deployed `config.json` and every
 * `PP_NODE_*` variable keep working. The node never sees the desktop's `AppConfig`.
 *
 * Today it is loaded through the app's config loader and projected (LoadNodeConfig); a loader of its own, reading only
 * these keys, comes with `capabilities/product` (CR028 step 2).
 */
struct NodeConfig {
  MeshConfig mesh;
  ServiceEndpointConfig registration;
  /** `data_dir`: the node's data root; empty, a `pp-node` folder beside the app's default one (PM007). */
  std::string data_dir;
  /** `relay.content_sha256`: send `x-amz-content-sha256` on relay POST/PUT. */
  bool relay_content_sha256 = false;
  /** `node`: the optional services' sections (projects/node-relay NR001), as the file has them. */
  Object node;
};

} // namespace pbr
