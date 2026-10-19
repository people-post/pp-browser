#pragma once

#include "mesh/shared/MeshExecutors.h"

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * pp-browser's executors for a mesh (THREADING.md § Mesh threads): `work` on the worker pool's Normal
 * lane, `blocking` on its Background lane, so a long wait never holds up short jobs. Every mesh the
 * product or pp-node starts takes these.
 */
mesh::MeshExecutors AppRuntimeMeshExecutors();

} // namespace pbr
